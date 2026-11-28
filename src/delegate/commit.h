// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The commit, once Hydra has synced the prims into their host copies: the
// render settings the render uses, which stay on the host, and the change
// set every device receives, the cameras, materials, prototypes, lights and
// textures added, updated and removed since the last commit
// (docs/adr/0013-devices-render-from-their-own-memory.md).

#ifndef UMBRA_DELEGATE_COMMIT_H_
#define UMBRA_DELEGATE_COMMIT_H_

#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

#include <expected>
#include <optional>
#include <span>
#include <string>

#include "hydra.h"
#include "ledger.h"
#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/device/device.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::delegate::internal {

// What a commit needs, besides the synced prims.
struct CommitInputs {
  // The RenderSettings prim the render uses.
  pxr::SdfPath settings;
  // The scene's file, which names the implicit product.
  std::string scene_path;
  // The frame, or none for the default time.
  std::optional<double> frame;
};

// Commits to `devices` what changed in the synced prims since the commit
// `ledger` records, and records this one, reading only the prims' host
// copies. Fails with kNotFound when no RenderSettings prim is at the
// settings' path, and kInvalidArgument when the settings author an invalid
// umbra:dicingRate, when the settings or a product have no camera or one that
// is not a Camera, a product has no orderedVars or none umbra renders, or a
// product's file is not .exr; kUnavailable when a file cannot be written.
// Unsupported render vars are left out, with one warning.
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
    const CommitInputs& inputs, SyncedPrims& prims, Ledger& ledger,
    std::span<device::Device* const> devices);

}  // namespace umbra::delegate::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DELEGATE_COMMIT_H_
