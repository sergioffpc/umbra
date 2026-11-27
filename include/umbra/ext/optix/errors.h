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
#include <string>
#include <string_view>

#include "umbra/core/log.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::optix {

// A value, or the message of the error that prevented it.
template <typename T>
using Result = std::expected<T, std::string>;

// Fails with OptiX's message when `result` is not OPTIX_SUCCESS:
// "OptiX: <what>: <message> (<name>)".
[[nodiscard]] Result<void> Check(OptixResult result, std::string_view what);

// Where the bridge logs what it cannot return: the messages of a context
// and of what it compiles. A function of the code it is linked into, which
// applies its own threshold.
using LogFunction = void (*)(core::LogLevel level, std::string_view message,
                             core::LogOrigin origin);

// Logs through `log` from now on; nothing is logged while it is null.
void SetLog(LogFunction UMBRA_NULLABLE log);

}  // namespace umbra::ext::optix

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_OPTIX_ERRORS_H_
