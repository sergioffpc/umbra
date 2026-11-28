// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/device/cpu/cpu_device.h"

#include <embree4/rtcore_device.h>

#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "arena.h"
#include "bvh.h"
#include "commit.h"
#include "copies.h"
#include "held.h"
#include "render.h"
#include "trace.h"
#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/device/camera_rays.h"
#include "umbra/device/change_set.h"
#include "umbra/device/device.h"
#include "umbra/device/film.h"
#include "umbra/device/id_table.h"
#include "umbra/device/render_data.h"
#include "umbra/ext/embree/owners.h"

namespace umbra::device::cpu {

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;
using ::umbra::device::cpu::internal::Status;

struct CpuDevice::State {
  explicit State(int node)
      : name(device::DeviceName(device::DeviceKind::kCpu, node)),
        arena(node, name) {}

  std::string name;
  internal::Arena arena;
  RTCDevice UMBRA_NULLABLE embree = ext::embree::ProcessDevice();
  internal::Held held;
  internal::Film film;
};

CpuDevice::CpuDevice(int node) : state_(std::make_unique<State>(node)) {}

CpuDevice::~CpuDevice() = default;

std::string_view CpuDevice::name() const { return state_->name; }

std::expected<void, Error> CpuDevice::Commit(const device::ChangeSet& changes) {
  State& state = *state_;
  RTCDevice UMBRA_NULLABLE embree = state.embree;
  if (embree == nullptr) {
    return std::unexpected(
        Error(ErrorCode::kInternal, "Embree is not initialized"));
  }
  // Not null: checked above.
  RTCDevice device = embree;
  Status status;
  state.arena.Execute([&state, &changes, device, &status] {
    status = internal::Commit(state.held, changes, device, state.name);
  });
  if (!status.has_value()) {
    // The BVHs may point to copies the failed commit replaced: nothing
    // traces them until a commit succeeds.
    state.held.top = ext::embree::Scene();
    state.held.placements.clear();
  }
  return status;
}

std::expected<void, Error> CpuDevice::Render(
    const device::FrameParameters& frame, device::TileQueue& tiles) {
  State& state = *state_;
  const std::optional<device::CameraModel> camera = device::FrameCameraModel(
      state.held.cameras, frame,
      [](const internal::Copy<device::Camera>& held) { return held.view; });
  if (!camera.has_value()) {
    return std::unexpected(Error(
        ErrorCode::kInternal, std::format("{} holds no camera {}", state.name,
                                          std::to_underlying(frame.camera))));
  }
  const std::optional<device::FilmLayout> layout = device::PlanFilm(frame);
  if (!layout.has_value()) {
    return std::unexpected(
        Error(ErrorCode::kInvalidArgument,
              std::format("{} renders at most {} vars, not {}", state.name,
                          device::kMaxFilmPlanes, frame.vars.size())));
  }
  Status status;
  state.arena.Execute([&] {
    internal::PrepareFilm(state.film, *layout, state.name);
    const internal::RenderInputs inputs{
        .device = this,
        .sampling =
            {
                .camera = &*camera,
                .sampler_cache = state.film.sampler_cache.data(),
                .samples_per_pixel = frame.samples_per_pixel,
            },
        .batch_size = static_cast<std::uint64_t>(state.arena.concurrency()),
    };
    status = internal::RenderBatches(
        inputs, frame, tiles, {.layout = &*layout, .film = state.film.planes});
  });
  return status;
}

std::optional<Hit> CpuDevice::Intersect(const Ray& ray) const
    [[clang::nonblocking]] {
  return internal::Intersect(state_->held.top.get(), state_->held.placements,
                             ray);
}

}  // namespace umbra::device::cpu
