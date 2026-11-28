// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// How a CPU device commits a change set: it applies its ids to what it
// holds, copies the prims it adds and updates, builds, refits or rebuilds
// their BVHs, and rebuilds its top level
// (docs/adr/0013-devices-render-from-their-own-memory.md).

#ifndef UMBRA_DEVICE_CPU_COMMIT_H_
#define UMBRA_DEVICE_CPU_COMMIT_H_

#include <embree4/rtcore_device.h>

#include <string_view>

#include "bvh.h"
#include "held.h"
#include "umbra/core/safety.h"
#include "umbra/device/change_set.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device::cpu::internal {

// Applies `changes` to `held`, with Embree's `device`, on the calling
// thread's arena, and logs, as the device `name`, what it built. Fails with
// kInternal when an id of `changes` does not apply to what `held` holds
// (device::ApplyIds), and with kUnavailable, with Embree's error, when
// Embree cannot build a BVH.
[[nodiscard]] Status Commit(Held& held, const device::ChangeSet& changes,
                            RTCDevice device, std::string_view name);

}  // namespace umbra::device::cpu::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_CPU_COMMIT_H_
