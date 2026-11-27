// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The commit, once Hydra has synced the prims into their host copies: the
// render settings the render uses, which stay on the host, and the change
// set every device receives, the cameras, materials, prototypes, lights and
// textures added, updated and removed since the last commit
// (docs/adr/0013-devices-render-from-their-own-memory.md).

#ifndef UMBRA_EXT_USD_COMMIT_H_
#define UMBRA_EXT_USD_COMMIT_H_

#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "host_copy.h"
#include "hydra.h"
#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/device/change_set.h"
#include "umbra/device/device.h"
#include "umbra/device/render_data.h"
#include "views.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::usd::internal {

// What a commit needs, besides the synced prims.
struct CommitInputs {
  // The RenderSettings prim the render uses.
  pxr::SdfPath settings;
  // The scene's file, which names the implicit product.
  std::string scene_path;
  // The frame, or none for the default time.
  std::optional<double> frame;
};

// The ids of one kind, dense: an id freed is taken again, but not before
// the next commit.
template <typename Id>
class IdPool {
 public:
  // A free id, or the next one.
  [[nodiscard]] Id Take() {
    if (!free_.empty()) {
      const Id taken = free_.back();
      free_.pop_back();
      return taken;
    }
    return static_cast<Id>(count_++);
  }

  // Frees `freed`, for a later commit.
  void Free(Id freed) { freed_.push_back(freed); }

  // Makes the ids freed by this commit free for the next.
  void Recycle() {
    free_.insert(free_.end(), freed_.begin(), freed_.end());
    freed_.clear();
  }

  // The number of ids: every id taken is below it.
  [[nodiscard]] std::uint32_t count() const { return count_; }

 private:
  std::uint32_t count_ = 0;
  std::vector<Id> free_;
  std::vector<Id> freed_;
};

// A prototype the devices hold: its id, and the keys of its materials.
struct HeldPrototype {
  device::PrototypeId id{};
  std::vector<std::string> materials;
};

// A material the devices hold: its id, the textures it reads, and the
// prototypes that read it.
struct HeldMaterial {
  device::MaterialId id{};
  std::vector<host::Texture> textures;
  std::size_t readers = 0;
};

// A light the devices hold: its id, and the textures it reads.
struct HeldLight {
  device::LightId id{};
  std::vector<host::Texture> textures;
};

// What the devices hold after a commit, which the next one starts from, so
// that it gives them only what changed: the id of each prim, by its path,
// and what it reads. A material is held by the path of its prim, or by the
// name of one of umbra's.
struct Ledger {
  IdPool<device::PrototypeId> prototype_ids;
  std::map<pxr::SdfPath, HeldPrototype> prototypes;
  IdPool<device::LightId> light_ids;
  std::map<pxr::SdfPath, HeldLight> lights;
  IdPool<device::MaterialId> material_ids;
  std::map<std::string, HeldMaterial> materials;
  IdPool<device::CameraId> camera_ids;
  std::map<pxr::SdfPath, device::CameraId> cameras;
  IdPool<device::TextureId> texture_ids;
  TextureIds textures;
  // The materials and lights that read each texture.
  std::map<host::Texture, std::size_t> texture_readers;
  // The shutter the prims were sampled over; none before the first commit.
  std::optional<host::Shutter> shutter;
  // The warnings logged: a later commit does not log them again.
  std::set<std::string> warnings;

  // The number of ids of each kind.
  [[nodiscard]] device::IdCounts id_counts() const {
    return {
        .prototypes = prototype_ids.count(),
        .lights = light_ids.count(),
        .materials = material_ids.count(),
        .cameras = camera_ids.count(),
        .textures = texture_ids.count(),
    };
  }

  // Makes the ids the commit freed free for the next.
  void Recycle() {
    prototype_ids.Recycle();
    light_ids.Recycle();
    material_ids.Recycle();
    camera_ids.Recycle();
    texture_ids.Recycle();
  }
};

// The scene a commit reads besides its prims: the stage, which tells which
// render vars author their dataType, and the terminal scene index, which
// tells which lights link geometry.
struct CommitScene {
  const pxr::UsdStage* stage;
  pxr::HdSceneIndexBaseRefPtr scene_index;
};

// Commits to `devices` what changed in the synced prims since the commit
// `ledger` records, and records this one, reading `scene`. Fails with kNotFound
// when no RenderSettings prim is at the settings' path, and kInvalidArgument
// when the settings author an invalid umbra:dicingRate, when the settings or a
// product have no camera or one that is not a Camera, a product has no
// orderedVars or none umbra renders, or a product's file is not .exr;
// kUnavailable when a file cannot be written. Unsupported render vars are left
// out, with one warning.
//
// The cameras committed are those the settings and their products see
// through: added when they first do, updated when they synced or the
// shutter changed, and removed when they no longer do. Every material prim
// is committed from its first sync to its destruction, updated when it
// syncs: as its network when it translates, and as the fallback material
// otherwise. Each mesh converted with an instance or more is a prototype,
// added when it becomes one, updated with what its sync converted, and
// removed when it is no longer one, with its materials, its own and those
// of its GeomSubsets: the one bound; with none bound, the display color
// material when the mesh has a displayColor, and the fallback material
// otherwise. umbra's two materials are committed while a prototype reads
// them. The lights converted with an instance or more are committed as the
// meshes are, updated whole: a PortalLight only while a dome light
// committed targets it, and a dome light with the ids of its portals. A
// texture is committed while a material or a light reads it, once.
//
// Fails with kInvalidArgument when one of the materials reads a texture or
// shader inside a package, or a light a texture. One warning each reports
// the materials that do not translate, the textures that do not resolve,
// the materials that connect each terminal but the surface, the invalid
// meshes, the rprims of each
// type not supported, the lights of each type not supported, the
// PortalLights no dome light targets, the dome lights that target a prim
// that is not a PortalLight as a portal, each issue of the lights and of
// the mesh lights (LightIssue), the lights that synced that link geometry,
// the prims whose topology changes within the shutter and those resampled
// down to
// kMaxMotionSamples, with their count and the first; a warning a commit
// logged before is not logged again. The frame's shutter is that the prims
// were sampled over. Then every device commits the change set, in
// parallel, each on a host thread of its own; the first, in order, that
// fails stops the commit, with its error, naming it.
[[nodiscard]] std::expected<Committed, core::Error> Commit(
    const CommitInputs& inputs, const CommitScene& scene, SyncedPrims& prims,
    Ledger& ledger, std::span<device::Device* const> devices);

}  // namespace umbra::ext::usd::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_USD_COMMIT_H_
