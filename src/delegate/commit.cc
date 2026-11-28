// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "commit.h"

#include <pxr/usd/sdf/path.h>

#include <expected>
#include <format>
#include <span>
#include <string>
#include <utility>

#include "commit_cameras.h"
#include "commit_description.h"
#include "commit_geometry.h"
#include "commit_lights.h"
#include "commit_materials.h"
#include "commit_products.h"
#include "commit_textures.h"
#include "commit_warnings.h"
#include "device_threads.h"
#include "hydra.h"
#include "ledger.h"
#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/delegate/render_settings.h"
#include "umbra/device/change_set.h"
#include "umbra/device/device.h"
#include "views.h"

namespace umbra::delegate::internal {
namespace {

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;

// One commit: the change set it builds from the synced prims and the
// ledger, and the warnings it reports.
class FrameCommit {
 public:
  FrameCommit(const CommitInputs& inputs UMBRA_LIFETIMEBOUND,
              const CommitScene& scene UMBRA_LIFETIMEBOUND,
              const SyncedPrims& prims UMBRA_LIFETIMEBOUND,
              Ledger& ledger UMBRA_LIFETIMEBOUND)
      : inputs_(&inputs),
        scene_(&scene),
        prims_(&prims),
        ledger_(&ledger),
        warnings_(ledger.warnings),
        changes_(ledger.textures),
        textures_(ledger.textures, changes_),
        materials_(prims, ledger, changes_, textures_),
        cameras_(prims, ledger, motion_, changes_,
                 ledger.shutter == prims.shutter ? ShutterChange::kSame
                                                 : ShutterChange::kChanged) {}
  FrameCommit(const FrameCommit&) = delete;
  FrameCommit& operator=(const FrameCommit&) = delete;
  FrameCommit(FrameCommit&&) = delete;
  FrameCommit& operator=(FrameCommit&&) = delete;
  ~FrameCommit() = default;

  // The render settings of `settings` and the frame parameters, with the
  // cameras they see through; the description is left empty.
  std::expected<Committed, Error> CommitSettings(
      const RenderSettingsPrim& settings);

  // Adds to the change set what changed in what the frame renders, its
  // geometry with its materials and its lights, the warnings about them
  // reported; or gives the error that stops the render.
  std::expected<void, Error> CommitContent();

  // The change set, valid while the committer and the host copies live.
  [[nodiscard]] device::ChangeSet Build() const UMBRA_LIFETIMEBOUND {
    return changes_.Build(ledger_->id_counts());
  }

  // What `change_set` commits, with `settings`: see Describe.
  [[nodiscard]] std::string DescribeCommit(
      const RenderSettings& settings,
      const device::ChangeSet& change_set) const;

 private:
  // Counts the motion of the instancers that synced.
  void TallyInstancers();

  const CommitInputs* inputs_;
  const CommitScene* scene_;
  const SyncedPrims* prims_;
  Ledger* ledger_;
  Warnings warnings_;
  ChangeSetBuilder changes_;
  Textures textures_;
  Materials materials_;
  MotionTallies motion_;
  Cameras cameras_;
  // The path of the camera the settings see through.
  std::string camera_;
};

// An invalid argument when `settings`, at `path`, have an invalid dicing
// rate.
std::expected<void, Error> CheckDicingRate(const RenderSettingsPrim& settings,
                                           const std::string& path) {
  if (const std::string& invalid = settings.dicing_rate().invalid;
      !invalid.empty()) {
    return std::unexpected(
        Error(ErrorCode::kInvalidArgument,
              std::format("render settings {}: {}", path, invalid)));
  }
  return {};
}

std::expected<Committed, Error> FrameCommit::CommitSettings(
    const RenderSettingsPrim& settings) {
  if (auto checked = CheckDicingRate(settings, inputs_->settings.GetString());
      !checked.has_value()) {
    return std::unexpected(std::move(checked.error()));
  }
  const pxr::SdfPath camera_path = settings.CameraPath();
  const auto camera = cameras_.Index(
      camera_path,
      std::format("render settings {}", inputs_->settings.GetString()));
  if (!camera.has_value()) {
    return std::unexpected(camera.error());
  }
  ProductConverter converter(*inputs_, *scene_->stage, cameras_);
  auto products = converter.Products(settings, *camera);
  converter.Report(warnings_);
  if (!products.has_value()) {
    return std::unexpected(products.error());
  }
  cameras_.RemoveUnseen();
  camera_ = camera_path.GetString();
  return Committed{
      .settings =
          {
              .path = inputs_->settings.GetString(),
              .camera = *camera,
              .products = std::move(*products),
          },
      .frame =
          {
              .shutter = prims_->shutter,
              .camera = *camera,
              .image = {},
              .motion_blur = MotionBlur::kEnabled,
              .depth_of_field = DepthOfField::kEnabled,
              .samples_per_pixel = 1,
              .vars = {},
          },
      .description = {},
  };
}

void FrameCommit::TallyInstancers() {
  for (const auto& [path, instancer] : prims_->instancers) {
    if (instancer->IsSynced()) {
      motion_.Add(path, instancer->motion_outcome());
    }
  }
}

std::expected<void, Error> FrameCommit::CommitContent() {
  materials_.CommitPrims();
  Lights lights(*prims_, *ledger_, motion_, textures_, changes_);
  lights.CommitAll();
  lights.RemoveGone(*prims_);
  Geometry geometry(*ledger_, motion_, materials_, changes_);
  for (const auto& [path, mesh] : prims_->meshes) {
    geometry.Commit(path, *mesh);
  }
  geometry.RemoveGone(*prims_);
  materials_.RemoveUnread();
  for (const auto& [path, prim] : prims_->unsupported) {
    geometry.Add(*prim);
  }
  TallyInstancers();
  lights.Report(warnings_);
  geometry.Report(warnings_);
  materials_.Report(warnings_);
  motion_.Report(warnings_);
  if (auto fatal = materials_.fatal().or_else([&] { return lights.fatal(); });
      fatal.has_value()) {
    return std::unexpected(std::move(*fatal));
  }
  ReportLightLinking(scene_->scene_index, *prims_, warnings_);
  return {};
}

std::string FrameCommit::DescribeCommit(
    const RenderSettings& settings, const device::ChangeSet& change_set) const {
  return Describe(settings, change_set, prims_->shutter,
                  {
                      .camera = camera_,
                      .textures = &ledger_->textures,
                      .display_color = materials_.DisplayColorId(),
                  });
}

}  // namespace

std::expected<Committed, Error> Commit(
    const CommitInputs& inputs, const CommitScene& scene, SyncedPrims& prims,
    Ledger& ledger, std::span<device::Device* const> devices) {
  const auto found = prims.settings.find(inputs.settings);
  if (found == prims.settings.end()) {
    return std::unexpected(
        Error(ErrorCode::kNotFound, std::format("no RenderSettings prim at {}",
                                                inputs.settings.GetString())));
  }
  FrameCommit commit(inputs, scene, prims, ledger);
  auto committed = commit.CommitSettings(*found->second);
  if (!committed.has_value()) {
    return committed;
  }
  if (auto content = commit.CommitContent(); !content.has_value()) {
    return std::unexpected(std::move(content.error()));
  }
  const device::ChangeSet change_set = commit.Build();
  committed->description =
      commit.DescribeCommit(committed->settings, change_set);
  const auto commit_on_device = [&change_set](device::Device& device) {
    return device.Commit(change_set);
  };
  if (auto done = OnEachDevice(devices, commit_on_device); !done.has_value()) {
    return std::unexpected(std::move(done.error()));
  }
  ledger.Recycle();
  ledger.shutter = prims.shutter;
  return committed;
}

}  // namespace umbra::delegate::internal
