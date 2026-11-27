// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// What umbra shows of its devices (docs/adr/0004-render-devices.md): the
// output of `umbra devices`, every device with the hardware behind it, and
// the devices of the block a render starts with
// (docs/adr/0002-cli-diagnostics-on-stderr.md).

#ifndef UMBRA_APPS_UMBRA_DEVICE_LISTING_H_
#define UMBRA_APPS_UMBRA_DEVICE_LISTING_H_

#include <span>
#include <string>

#include "devices.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::devices {

// The output of `umbra devices`: every device and the hardware behind it,
// and the GPUs umbra cannot use, with the reason.
[[nodiscard]] std::string FormatHardware(const Hardware& hardware);

// The lines of the block a render starts with, one per device in `chosen`:
// "  cpu0  AMD Ryzen Threadripper PRO 3975WX, 64 threads",
// "  gpu0  NVIDIA GeForce RTX 5070 Ti, 15.5 GiB".
[[nodiscard]] std::string FormatRenderDevices(
    const Hardware& hardware, std::span<const RenderDevice> chosen);

}  // namespace umbra::devices

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_APPS_UMBRA_DEVICE_LISTING_H_
