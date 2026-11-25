// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The Scene: what umbra renders, in umbra's own types, with no OpenUSD type
// (docs/adr/0006-scenes-enter-through-hydra.md). The OpenUSD bridge builds it
// (umbra::ext::usd::LoadScene) and it is immutable once built.

#ifndef UMBRA_SCENE_SCENE_H_
#define UMBRA_SCENE_SCENE_H_

#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::scene {

// The scene of one frame. It is empty.
class Scene {
 public:
  Scene();
};

}  // namespace umbra::scene

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_SCENE_SCENE_H_
