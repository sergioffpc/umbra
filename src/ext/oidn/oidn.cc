// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/ext/oidn/oidn.h"

#include <OpenImageDenoise/config.h>

#include <expected>

#include "umbra/core/error.h"
#include "umbra/core/log.h"

namespace umbra::ext::oidn {

std::expected<void, core::Error> Initialize() {
  UMBRA_LOG_DEBUG(kOidn, "Open Image Denoise {}", OIDN_VERSION_STRING);
  return {};
}

}  // namespace umbra::ext::oidn
