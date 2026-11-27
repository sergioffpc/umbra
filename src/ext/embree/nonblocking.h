// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The entry points of Embree that the CPU device's hot path calls, declared
// again as nonblocking (docs/adr/0003-no-allocations-on-the-hot-path.md).
// They are compiled in Embree's library, so clang cannot infer what they do:
// this header is where umbra trusts them, each for the reason it gives. The
// RealtimeSanitizer checks that trust at run time, inside Embree too.
//
// Include it after Embree's headers, in every file whose hot path calls
// them.

#ifndef UMBRA_EXT_EMBREE_NONBLOCKING_H_
#define UMBRA_EXT_EMBREE_NONBLOCKING_H_

#include <embree4/rtcore_common.h>
#include <embree4/rtcore_ray.h>
#include <embree4/rtcore_scene.h>

#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

// NOLINTBEGIN(readability-redundant-declaration): Embree's, with the effect.

// Traces one ray through a committed scene: it reads the BVHs and calls the
// geometries' callbacks, umbra's own nonblocking ones, on the calling thread.
extern "C" void rtcIntersect1(RTCScene, struct RTCRayHit*,
                              struct RTCIntersectArguments* UMBRA_NULLABLE)
    [[clang::nonblocking]];

// Reads an instance's transform at a time from a committed scene.
extern "C" void rtcGetGeometryTransformFromScene(RTCScene, unsigned int, float,
                                                 enum RTCFormat, void*)
    [[clang::nonblocking]];

// NOLINTEND(readability-redundant-declaration)

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_EMBREE_NONBLOCKING_H_
