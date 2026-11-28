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
#include <format>
#include <string>

#include "umbra/core/lazy_format.h"
#include "umbra/core/log_hook.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::cuda {

// A value, or the message of the error that prevented it.
template <typename T>
using Result = std::expected<T, std::string>;

// Fails with the CUDA runtime's message when `error` is not cudaSuccess:
// "CUDA: <what>: <message> (<name>)". `what` is formatted only then.
[[nodiscard]] Result<void> Check(cudaError_t error,
                                 const core::LazyFormat& what);

// Fails as above, with `what` formatted with `args` only when `error` is not
// cudaSuccess. Every call of the bridge that takes `what` and its `args`
// formats them only when it fails, so that a call that succeeds allocates
// nothing.
template <typename... Args>
[[nodiscard]] Result<void> Check(cudaError_t error,
                                 std::format_string<Args...> what,
                                 const Args&... args) {
  return Check(error,
               core::LazyFormat(what.get(), std::make_format_args(args...)));
}

// Where the bridge logs what it cannot return, such as a release that fails
// in a destructor: a function of the code it is linked into. Logs
// through `log` from now on, the messages of `threshold` and more severe
// ones; nothing is logged while it is null.
void SetLog(core::LogFunction UMBRA_NULLABLE log, core::LogLevel threshold);

}  // namespace umbra::ext::cuda

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_CUDA_ERRORS_H_
