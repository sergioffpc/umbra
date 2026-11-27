// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The description of a commit, which the debug log gives: what it added,
// counted, and how many prims it updated and removed.

#ifndef UMBRA_DELEGATE_COMMIT_DESCRIPTION_H_
#define UMBRA_DELEGATE_COMMIT_DESCRIPTION_H_

#include <optional>
#include <string>
#include <string_view>

#include "ledger.h"
#include "umbra/core/safety.h"
#include "umbra/delegate/render_settings.h"
#include "umbra/device/change_set.h"
#include "umbra/device/device.h"
#include "umbra/device/render_data.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::delegate::internal {

// What a commit added, which the description counts, besides the change
// set.
struct Description {
  // The path of the camera the settings see through.
  std::string_view camera;
  // The textures held, by which the materials' are counted by file.
  const HeldTextures* textures;
  // The id of the display color material, when it is held.
  std::optional<device::MaterialId> display_color;
};

// "settings /Render/main, camera /World/cam, 1 product: render/beauty.0012.exr
// [color, depth], 1.24M triangles in 3.4k meshes, 18 primvars, 2.1M
// instances of 3.4k prototypes, 57 materials (MaterialX 40,
// UsdPreviewSurface 15, OSL 2), 210 textures, 300 meshes on displayColor,
// 14 lights (rect 8, sphere 4, distant 1, dome 1), shutter -0.25..0.25, 1.2k
// moving (max 4 samples), 2 prims updated, 0 removed": what the commit of
// `settings` and `changes` added, its frame's shutter `shutter`, then how
// many prims it updated and removed.
[[nodiscard]] std::string Describe(const RenderSettings& settings,
                                   const device::ChangeSet& changes,
                                   const device::Shutter& shutter,
                                   const Description& description);

}  // namespace umbra::delegate::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DELEGATE_COMMIT_DESCRIPTION_H_
