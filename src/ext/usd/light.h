// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// UsdLux lights, as Hydra gives them, synced into the Scene's lights: their
// shapes in meters, their colors in ACEScg
// (docs/adr/0007-spectral-rendering-from-acescg.md), their textures resolved
// to absolute files with their color spaces, and their transforms sampled
// over the shutter. The controls without a physical counterpart are not
// taken, only noted (docs/adr/0010-lights-are-physical.md).

#ifndef UMBRA_EXT_USD_LIGHT_H_
#define UMBRA_EXT_USD_LIGHT_H_

#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/light.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "motion.h"
#include "umbra/core/error.h"
#include "umbra/core/progress.h"
#include "umbra/core/safety.h"
#include "umbra/scene/scene.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::usd::internal {

namespace pxr = PXR_NS;

// The light sprim types the delegate syncs: UsdLux's six kinds, and the
// generic light and plugin light types, which umbra does not take, only
// counts.
[[nodiscard]] const pxr::TfTokenVector& LightPrimTypes();

// Whether the prim at `prim_id` is a light Hydra gives as another prim: a
// mesh with UsdLux's MeshLightAPI.
[[nodiscard]] bool IsLight(pxr::HdSceneDelegate& delegate,
                           const pxr::SdfPath& prim_id);

// What the sync of a light gave.
enum class LightState : std::uint8_t {
  // It is invisible, or of a purpose the settings do not include.
  kHidden,
  // It is converted into the Scene's, with its instances.
  kConverted,
  // It is of a kind umbra does not take (PortalLight, GeometryLight,
  // PluginLight): only counted.
  kUnsupported,
};

// What a light authors that umbra does not take, for the warnings that
// report it.
enum class LightIssue : std::uint8_t {
  // It has light filters.
  kFilters,
  // Its emission is shaped by an IES profile.
  kIesProfile,
  // It enables a color temperature that is not above 0 K: its color is used
  // alone.
  kNonPositiveTemperature,
  // Its diffuse or specular multiplier is not 1.
  kDiffuseSpecular,
  // It focuses its emission (shaping:focus).
  kFocus,
  // It softens the edge of its cone.
  kConeSoftness,
  // It authors ShadowAPI's controls.
  kShadowControls,
  // Its color does not convert into ACEScg: it is white.
  kColorNotConverted,
  // Its texture is not read: it does not resolve, or its color space is
  // unknown.
  kTextureNotRead,
};

// One issue of a light, with what the warning says of it.
struct LightNote {
  LightIssue issue = LightIssue::kFilters;
  std::string detail;
};

// A light sprim, synced. It reads its parameters as it syncs, before the
// settings choose the shutter and the purposes, and is placed once they
// have: its transform sampled over the shutter, one light for each
// instance of its instancer.
class LightPrim final : public pxr::HdLight {
 public:
  // Counts its sync into `phase`, unless it is a fallback prim; its lengths
  // are scaled by `meters_per_unit`.
  LightPrim(pxr::TfToken type_id, const pxr::SdfPath& prim_id,
            core::ProgressPhase* UMBRA_NULLABLE phase, double meters_per_unit);

  [[nodiscard]] pxr::HdDirtyBits GetInitialDirtyBitsMask() const override;
  void Sync(pxr::HdSceneDelegate* scene_delegate,
            pxr::HdRenderParam* UMBRA_NULLABLE render_param,
            pxr::HdDirtyBits* dirty_bits) override;

  // Places it over `shutter`, once synced: hidden when its purpose's render
  // tag is not in `render_tags`.
  void Place(const scene::Shutter& shutter,
             const pxr::TfTokenVector& render_tags);

  [[nodiscard]] LightState state() const { return state_; }
  // The UsdLux type of a light umbra does not take ("PortalLight").
  [[nodiscard]] const std::string& unsupported_type() const
      UMBRA_LIFETIMEBOUND {
    return unsupported_type_;
  }
  // What it authors that umbra does not take, once converted.
  [[nodiscard]] const std::vector<LightNote>& notes() const
      UMBRA_LIFETIMEBOUND {
    return notes_;
  }
  // The error that stops the render: a texture inside a package.
  [[nodiscard]] const std::optional<core::Error>& fatal() const
      UMBRA_LIFETIMEBOUND {
    return fatal_;
  }
  // What sampling did with its motion, once placed.
  [[nodiscard]] MotionOutcome motion_outcome() const { return motion_outcome_; }
  // Its lights, once placed, one for each of its instances, moved out.
  [[nodiscard]] std::vector<scene::Light> TakeLights();

 private:
  void Convert(pxr::HdSceneDelegate& delegate);

  pxr::TfToken type_;
  core::ProgressPhase* UMBRA_NULLABLE phase_;
  double meters_per_unit_;
  // The scene delegate it synced with, which Place reads: Hydra's, which
  // lives as long as the render index, past the sync that closes the Scene.
  pxr::HdSceneDelegate* UMBRA_NULLABLE delegate_ = nullptr;
  LightState state_ = LightState::kHidden;
  pxr::TfToken render_tag_;
  std::string unsupported_type_;
  std::vector<LightNote> notes_;
  std::optional<core::Error> fatal_;
  MotionOutcome motion_outcome_ = MotionOutcome::kSampled;
  // Everything but its transforms.
  scene::Light light_;
  std::vector<scene::Light> lights_;
};

}  // namespace umbra::ext::usd::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_USD_LIGHT_H_
