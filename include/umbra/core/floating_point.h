// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#ifndef UMBRA_CORE_FLOATING_POINT_H_
#define UMBRA_CORE_FLOATING_POINT_H_

#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::core {

// Configures the floating-point state of the calling thread: denormal results
// flush to zero (FTZ) and denormal inputs read as zero (DAZ), so denormal
// handling is the same everywhere.
//
// Call it first on every thread, tests and benchmarks included; the state is
// per thread and is not inherited by new threads.
void ConfigureThreadFloatingPoint();

}  // namespace umbra::core

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_CORE_FLOATING_POINT_H_
