// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// One host thread per device: how the delegate has every device commit
// and render at the same time.

#ifndef UMBRA_DELEGATE_DEVICE_THREADS_H_
#define UMBRA_DELEGATE_DEVICE_THREADS_H_

#include <expected>
#include <functional>
#include <span>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/device/device.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::delegate::internal {

// What a device does on its thread.
using DeviceWork =
    std::function<std::expected<void, core::Error>(device::Device&)>;

// Runs `work` on every device of `devices` in parallel, each on a host
// thread of its own whose floating point is configured first (FTZ/DAZ).
// Once all are done, fails with the error of the first, in their order,
// that fails, naming its device.
[[nodiscard]] std::expected<void, core::Error> OnEachDevice(
    std::span<device::Device* const> devices, const DeviceWork& work);

}  // namespace umbra::delegate::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DELEGATE_DEVICE_THREADS_H_
