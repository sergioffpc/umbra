// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The process's Embree device, which every CPU device builds its BVHs with.

#ifndef UMBRA_EXT_EMBREE_PROCESS_DEVICE_H_
#define UMBRA_EXT_EMBREE_PROCESS_DEVICE_H_

#include <embree4/rtcore_device.h>

#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::embree::internal {

// The device Initialize created; null before it.
[[nodiscard]] RTCDevice UMBRA_NULLABLE ProcessRtcDevice();

}  // namespace umbra::ext::embree::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_EMBREE_PROCESS_DEVICE_H_
