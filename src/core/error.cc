// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/core/error.h"

#include <string>
#include <utility>

namespace umbra::core {

Error::Error(ErrorCode code, std::string message)
    : code_(code), message_(std::move(message)) {}

}  // namespace umbra::core
