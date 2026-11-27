// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The CUDA runtime's errors as umbra's, and where the bridge logs what it
// cannot return. The bridge is linked into the GPU module, which links no
// umbra module: its errors are messages, as the module's are, and it logs
// through the function the module hands it.

#ifndef UMBRA_EXT_CUDA_ERRORS_H_
#define UMBRA_EXT_CUDA_ERRORS_H_

#include <driver_types.h>

#include <expected>
#include <string>
#include <string_view>

#include "umbra/core/log.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::cuda {

// A value, or the message of the error that prevented it.
template <typename T>
using Result = std::expected<T, std::string>;

// Fails with the CUDA runtime's message when `error` is not cudaSuccess:
// "CUDA: <what>: <message> (<name>)".
[[nodiscard]] Result<void> Check(cudaError_t error, std::string_view what);

// Where the bridge logs what it cannot return, such as a release that fails
// in a destructor: a function of the code it is linked into, which applies
// its own threshold.
using LogFunction = void (*)(core::LogLevel level, std::string_view message,
                             core::LogOrigin origin);

// Logs through `log` from now on; nothing is logged while it is null.
void SetLog(LogFunction UMBRA_NULLABLE log);

}  // namespace umbra::ext::cuda

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_CUDA_ERRORS_H_
