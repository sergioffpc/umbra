// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// Owners of Embree's handles: the process's device, which Initialize
// creates, and the scenes and geometries built with it.

#ifndef UMBRA_EXT_EMBREE_OWNERS_H_
#define UMBRA_EXT_EMBREE_OWNERS_H_

#include <embree4/rtcore_device.h>
#include <embree4/rtcore_geometry.h>
#include <embree4/rtcore_scene.h>

#include "umbra/core/handle_owner.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::embree {

// The device Initialize created; null before it.
[[nodiscard]] RTCDevice UMBRA_NULLABLE ProcessDevice();

// An Embree scene, released when it is destroyed.
using Scene = core::HandleOwner<RTCScene, rtcReleaseScene>;

// An Embree geometry, released when it is destroyed. A scene it is attached
// to holds a reference of its own.
using Geometry = core::HandleOwner<RTCGeometry, rtcReleaseGeometry>;

}  // namespace umbra::ext::embree

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_EMBREE_OWNERS_H_
