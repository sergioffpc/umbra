// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// OptiX's errors as umbra's, and where the bridge logs what it cannot
// return. The bridge is linked into the GPU module, which links no umbra
// module: its errors are messages, as the module's are, and it logs through
// the function the module hands it.

#ifndef UMBRA_EXT_OPTIX_ERRORS_H_
#define UMBRA_EXT_OPTIX_ERRORS_H_

#include <optix_types.h>

#include <expected>
#include <format>
#include <string>

#include "umbra/core/lazy_format.h"
#include "umbra/core/log_hook.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::optix {

// A value, or the message of the error that prevented it.
template <typename T>
using Result = std::expected<T, std::string>;

// Fails with OptiX's message when `result` is not OPTIX_SUCCESS:
// "OptiX: <what>: <message> (<name>)". `what` is formatted only then.
[[nodiscard]] Result<void> Check(OptixResult result,
                                 const core::LazyFormat& what);

// Fails as above, with `what` formatted with `args` only when `result` is
// not OPTIX_SUCCESS. Every call of the bridge that takes `what` and its
// `args` formats them only when it fails, so that a call that succeeds
// allocates nothing.
template <typename... Args>
[[nodiscard]] Result<void> Check(OptixResult result,
                                 std::format_string<Args...> what,
                                 const Args&... args) {
  return Check(result,
               core::LazyFormat(what.get(), std::make_format_args(args...)));
}

// Where the bridge logs what it cannot return: the messages of a context
// and of what it compiles. A function of the code it is linked into.
// Logs through `log` from now on, the messages of `threshold` and more
// severe ones; nothing is logged while it is null.
void SetLog(core::LogFunction UMBRA_NULLABLE log, core::LogLevel threshold);

}  // namespace umbra::ext::optix

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_OPTIX_ERRORS_H_
