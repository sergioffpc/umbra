// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The bridge to Embree, umbra's CPU ray tracer. The process has one Embree
// device, which runs on the oneTBB pool (Embree's tasking system is TBB) and
// reports its errors to umbra's log as the embree component. The CPU device
// builds its BVHs with it, through owners.h, errors.h, conversions.h and
// nonblocking.h.

#ifndef UMBRA_EXT_EMBREE_EMBREE_H_
#define UMBRA_EXT_EMBREE_EMBREE_H_

#include <expected>
#include <string_view>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::embree {

// Initializes Embree for the process: creates its device, routes the device's
// errors into umbra's log, and logs the version at info. Call it once at
// startup, after core::ConfigureLogging. Fails with kUnavailable when the
// device cannot be created (an unsupported CPU, say).
[[nodiscard]] std::expected<void, core::Error> Initialize();

// The ISA Embree chose for the process's device, from the widest ray packet
// it handles natively: "AVX-512" or "AVX2" (the ISAs umbra builds Embree
// for), or "none" before Initialize.
[[nodiscard]] std::string_view NativeIsa();

}  // namespace umbra::ext::embree

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_EMBREE_EMBREE_H_
