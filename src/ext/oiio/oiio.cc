// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/ext/oiio/oiio.h"

#include <OpenImageIO/imageio.h>
#include <OpenImageIO/nsversions.h>
#include <OpenImageIO/typedesc.h>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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

// "cannot write '<file>': <reason>".
Error CannotWrite(std::string_view file, std::string_view reason) {
  return {ErrorCode::kUnavailable,
          std::format("cannot write '{}': {}", file, reason)};
}

// The channels of an image: their names, the format of the spec and, when
// they differ, each channel's format.
struct Channels {
  std::vector<std::string> names;
  OIIO::TypeDesc format = OIIO::TypeDesc::FLOAT;
  std::vector<OIIO::TypeDesc> formats;
};

// The channels of `image`. When every channel has the same format, that
// format is the spec's and `formats` is empty, since OpenImageIO's OpenEXR
// writer ignores per-channel formats that are all equal and writes the
// spec's format. When they differ, they are in `formats` and the spec's
// format is float, the most precise of them.
Channels ChannelsOf(const ExrImage& image) {
  Channels channels;
  for (const ExrLayer& layer : image.layers) {
    for (const std::string_view channel : layer.channels) {
      channels.names.push_back(std::format("{}.{}", layer.name, channel));
      channels.formats.emplace_back(layer.format == ChannelFormat::kHalf
                                        ? OIIO::TypeDesc::HALF
                                        : OIIO::TypeDesc::FLOAT);
    }
  }
  const std::vector<OIIO::TypeDesc>& formats = channels.formats;
  const bool is_uniform =
      std::ranges::all_of(formats, [&formats](const OIIO::TypeDesc& format) {
        return format == formats.front();
      });
  if (is_uniform && !formats.empty()) {
    channels.format = formats.front();
    channels.formats.clear();
  }
  return channels;
}

// The spec of `image`: its windows, its channels and its metadata.
OIIO::ImageSpec SpecOf(const ExrImage& image) {
  Channels channels = ChannelsOf(image);
  OIIO::ImageSpec spec(image.data_size.at(0), image.data_size.at(1),
                       static_cast<int>(channels.names.size()),
                       channels.format);
  spec.x = image.data_origin.at(0);
  spec.y = image.data_origin.at(1);
  spec.full_x = 0;
  spec.full_y = 0;
  spec.full_width = image.resolution.at(0);
  spec.full_height = image.resolution.at(1);
  spec.channelnames = std::move(channels.names);
  spec.channelformats = std::move(channels.formats);
  spec.attribute("compression", "zip");
  spec.attribute("worldtocamera", OIIO::TypeMatrix44,
                 image.world_to_camera.data());
  spec.attribute("worldtoNDC", OIIO::TypeMatrix44, image.world_to_ndc.data());
  spec.attribute("PixelAspectRatio", image.pixel_aspect_ratio);
  spec.attribute("software", image.software);
  if (image.frame.has_value()) {
    const double frame = *image.frame;
    spec.attribute("frame", OIIO::TypeDesc::DOUBLE, &frame);
  }
  return spec;
}

// The pixels of `image`'s layers, interleaved: every channel of a pixel,
// layer after layer, one pixel after another.
std::vector<float> Interleaved(const ExrImage& image, std::size_t channels) {
  const std::size_t pixels = static_cast<std::size_t>(image.data_size.at(0)) *
                             static_cast<std::size_t>(image.data_size.at(1));
  std::vector<float> interleaved(pixels * channels);
  const std::span<float> out = interleaved;
  std::size_t first = 0;
  for (const ExrLayer& layer : image.layers) {
    const std::size_t count = layer.channels.size();
    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
      std::ranges::copy(layer.pixels.subspan(pixel * count, count),
                        out.subspan((pixel * channels) + first, count).begin());
    }
    first += count;
  }
  return interleaved;
}

// Writes `image` to `path`.
std::expected<void, Error> WriteTo(const ExrImage& image,
                                   const std::filesystem::path& path) {
  const OIIO::ImageSpec spec = SpecOf(image);
  const std::vector<float> pixels =
      Interleaved(image, static_cast<std::size_t>(spec.nchannels));
  const std::unique_ptr<OIIO::ImageOutput> output =
      OIIO::ImageOutput::create("openexr");
  if (output == nullptr) {
    return std::unexpected(CannotWrite(image.file, OIIO::geterror()));
  }
  if (!output->open(path.string(), spec) ||
      !output->write_image(OIIO::TypeDesc::FLOAT, pixels.data()) ||
      !output->close()) {
    return std::unexpected(CannotWrite(image.file, output->geterror()));
  }
  return {};
}

}  // namespace

std::expected<void, Error> WriteExr(const ExrImage& image) {
  for (const ExrLayer& layer : image.layers) {
    const std::size_t expected =
        static_cast<std::size_t>(image.data_size.at(0)) *
        static_cast<std::size_t>(image.data_size.at(1)) * layer.channels.size();
    if (layer.pixels.size() != expected) {
      return std::unexpected(
          Error(ErrorCode::kInternal,
                std::format("{}: layer {} has {} values, not {}", image.file,
                            layer.name, layer.pixels.size(), expected)));
    }
  }
  const std::filesystem::path file(image.file);
  const std::filesystem::path temporary(std::string(image.file) + ".tmp");
  std::error_code error;
  if (file.has_parent_path()) {
    std::filesystem::create_directories(file.parent_path(), error);
    if (error) {
      return std::unexpected(CannotWrite(image.file, error.message()));
    }
  }
  std::expected<void, Error> written;
  try {
    written = WriteTo(image, temporary);
  } catch (const std::exception& exception) {
    written = std::unexpected(CannotWrite(image.file, exception.what()));
  }
  if (written.has_value()) {
    std::filesystem::rename(temporary, file, error);
    if (!error) {
      return {};
    }
    written = std::unexpected(CannotWrite(image.file, error.message()));
  }
  std::filesystem::remove(temporary, error);
  return written;
}

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
  UMBRA_LOG_DEBUG(kOiio, "OpenImageIO {}.{}.{}", version / 10000,
                  version / 100 % 100, version % 100);
  return {};
}

}  // namespace umbra::ext::oiio
