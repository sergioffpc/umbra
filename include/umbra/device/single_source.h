// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// What single-source code is marked with: functions written once, compiled
// by clang for the CPU devices and by nvcc for the GPU devices. They hold to
// what nvcc accepts: no exceptions, no virtual calls and no allocations
// (docs/adr/0003-no-allocations-on-the-hot-path.md).

#ifndef UMBRA_DEVICE_SINGLE_SOURCE_H_
#define UMBRA_DEVICE_SINGLE_SOURCE_H_

#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

// Marks a function that runs on the host and on a GPU.
// NOLINTBEGIN(cppcoreguidelines-macro-usage): CUDA's execution space
// specifiers are keywords that only nvcc knows.
#ifdef __CUDACC__
#define UMBRA_HOST_DEVICE __host__ __device__
#else
#define UMBRA_HOST_DEVICE
#endif
// NOLINTEND(cppcoreguidelines-macro-usage)

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_SINGLE_SOURCE_H_
