// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// What a CPU device logs of each commit, as the device component at debug
// level: the BVHs it built, refitted, rebuilt and dropped, and its top
// level.

#ifndef UMBRA_DEVICE_CPU_COMMIT_LOG_H_
#define UMBRA_DEVICE_CPU_COMMIT_LOG_H_

#include <span>
#include <string_view>

#include "held.h"
#include "umbra/core/safety.h"
#include "umbra/device/change_set.h"
#include "umbra/device/render_data.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device::cpu::internal {

// Logs, as the device `name`, the BVH of each prototype of `removed` that
// `held` holds, which the commit drops.
void LogDropped(std::string_view name, const Held& held,
                std::span<const device::PrototypeId> removed);

// Logs, as the device `name`, what the commit of `changes` built into
// `held`.
void LogCommit(std::string_view name, const Held& held,
               const device::ChangeSet& changes);

}  // namespace umbra::device::cpu::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_CPU_COMMIT_LOG_H_
