// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/ext/oiio/oiio.h"

#include <OpenImageIO/imageio.h>

#include <expected>
#include <format>
#include <string_view>

#include "umbra/core/error.h"
#include "umbra/core/log.h"

namespace umbra::ext::oiio {
namespace {

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;

// Sets a global integer attribute of OpenImageIO.
std::expected<void, Error> SetAttribute(std::string_view name, int value) {
  if (!OIIO::attribute(name, value)) {
    return std::unexpected(
        Error(ErrorCode::kUnavailable,
              std::format("OpenImageIO: cannot set {} to {}: {}", name, value,
                          OIIO::geterror())));
  }
  return {};
}

}  // namespace

std::expected<void, Error> Initialize() {
  // Parallel loops run on the process's oneTBB pool, and OpenEXR decodes on
  // the calling thread instead of a pool of its own (-1: none).
  auto configured = SetAttribute("use_tbb", 1).and_then([] {
    return SetAttribute("exr_threads", -1);
  });
  if (!configured.has_value()) {
    return configured;
  }
  const int version = OIIO::openimageio_version();
  UMBRA_LOG_INFO(kOiio, "OpenImageIO {}.{}.{}", version / 10000,
                 version / 100 % 100, version % 100);
  return {};
}

}  // namespace umbra::ext::oiio
