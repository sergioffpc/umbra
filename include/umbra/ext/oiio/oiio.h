// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The bridge to OpenImageIO, which reads and writes umbra's images: it runs
// on the oneTBB pool (docs/adr/0001-onetbb-thread-pool.md), with OpenEXR's own
// thread pool off. It writes the render products as OpenEXR files.

#ifndef UMBRA_EXT_OIIO_OIIO_H_
#define UMBRA_EXT_OIIO_OIIO_H_

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::oiio {

// Initializes OpenImageIO for the process: parallel loops on oneTBB, no
// OpenEXR thread pool, and its version logged at info. Call it once at
// startup, after core::ConfigureLogging. Fails with kUnavailable when
// OpenImageIO rejects a setting.
[[nodiscard]] std::expected<void, core::Error> Initialize();

// The type of a channel in a file.
enum class ChannelFormat : std::uint8_t {
  kHalf,
  kFloat,
};

// A layer of an OpenEXR file: channels named "<layer>.<channel>", each of
// one format.
struct ExrLayer {
  std::string_view name;
  // The names of its channels, after the layer's ("R", "G", ...).
  std::span<const std::string_view> channels;
  ChannelFormat format = ChannelFormat::kHalf;
  // Its values: as many per pixel as it has channels, over the data window,
  // the pixels row by row from its top left.
  std::span<const float> pixels;
};

// A single-part scanline OpenEXR file, as WriteExr writes it.
struct ExrImage {
  std::string_view file;
  // The display window: from (0, 0) to the resolution, width then height.
  std::array<std::int32_t, 2> resolution{};
  // The data window: its top left pixel, and its width and height.
  std::array<std::int32_t, 2> data_origin{};
  std::array<std::int32_t, 2> data_size{};
  std::span<const ExrLayer> layers;
  // worldToCamera and worldToNDC, row-major for row vectors.
  std::array<float, 16> world_to_camera{};
  std::array<float, 16> world_to_ndc{};
  float pixel_aspect_ratio = 1.0F;
  // What wrote it: its software attribute.
  std::string_view software;
  // Its frame attribute, none at the default time.
  std::optional<double> frame;
};

// Writes `image`, ZIP compressed, with its layers in order: into a file
// named after it with .tmp appended, in its directory, created with its
// missing parents, which then replaces it. Fails with kUnavailable when it
// cannot, the file left as it was.
[[nodiscard]] std::expected<void, core::Error> WriteExr(const ExrImage& image);

}  // namespace umbra::ext::oiio

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_OIIO_OIIO_H_
