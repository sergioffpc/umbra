// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "product_file.h"

#include <algorithm>
#include <array>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "channels.h"
#include "umbra/core/error.h"
#include "umbra/core/version.h"
#include "umbra/delegate/render_settings.h"
#include "umbra/delegate/session.h"
#include "umbra/device/render_data.h"
#include "umbra/ext/oiio/oiio.h"

namespace umbra {
namespace {

// `matrix` in float.
std::array<float, 16> FloatMatrix(const device::Matrix& matrix) {
  std::array<float, 16> floats{};
  std::ranges::transform(matrix, floats.begin(), [](double value) {
    return static_cast<float>(value);
  });
  return floats;
}

}  // namespace

std::expected<void, core::Error> WriteProduct(
    const delegate::RenderedProduct& rendered, std::optional<double> frame) {
  using ext::oiio::ChannelFormat;
  const delegate::RenderProduct& product = *rendered.product;
  std::vector<ext::oiio::ExrLayer> layers;
  layers.reserve(rendered.vars.size());
  for (const delegate::RenderedVar& var : rendered.vars) {
    layers.push_back({
        .name = var.var->name,
        .channels = ChannelNames(var.var->source, var.var->channels),
        .format = var.var->type == delegate::ChannelType::kHalf
                      ? ChannelFormat::kHalf
                      : ChannelFormat::kFloat,
        .pixels = var.pixels,
    });
  }
  const std::string software = std::format("umbra {}", core::Version());
  return ext::oiio::WriteExr({
      .file = product.file,
      .resolution = product.resolution,
      .data_origin = {product.window.x_min, product.window.y_min},
      .data_size = {product.window.width(), product.window.height()},
      .layers = layers,
      .world_to_camera = FloatMatrix(product.world_to_camera),
      .world_to_ndc = FloatMatrix(product.world_to_ndc),
      .pixel_aspect_ratio = product.file_pixel_aspect_ratio,
      .software = software,
      .frame = frame,
  });
}

}  // namespace umbra
