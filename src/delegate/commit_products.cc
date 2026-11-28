// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "commit_products.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/renderSettings.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usdRender/tokens.h>

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "commit.h"
#include "commit_cameras.h"
#include "commit_warnings.h"
#include "gf.h"
#include "host_copy.h"
#include "hydra.h"
#include "outputs.h"
#include "plural.h"
#include "render_vars.h"
#include "umbra/core/error.h"
#include "umbra/delegate/render_settings.h"
#include "umbra/device/camera_rays.h"
#include "umbra/device/image.h"
#include "umbra/device/render_data.h"
#include "views.h"

namespace umbra::delegate::internal {
namespace {

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;
using Product = pxr::HdRenderSettings::RenderProduct;

// The quantity of a raw var named `name`, or none when umbra has no such
// var.
std::optional<RenderVarSource> RawSource(std::string_view name) {
  for (const RenderVarTraits& var : kRenderVars) {
    if (var.kind == RenderVarKind::kRaw && var.name == name) {
      return var.source;
    }
  }
  return std::nullopt;
}

// The quantity of a var of `source_type` whose source is `source_name`, or
// none when umbra has no such var: an lpe or primvar var, unchecked, or a
// raw var umbra knows. An intrinsic var, which UsdRender leaves
// unimplemented, has none.
std::optional<RenderVarSource> SourceOf(const pxr::TfToken& source_type,
                                        std::string_view source_name) {
  if (source_type == pxr::UsdRenderTokens->lpe) {
    return RenderVarSource::kLpe;
  }
  if (source_type == pxr::UsdRenderTokens->primvar) {
    return RenderVarSource::kPrimvar;
  }
  if (source_type == pxr::UsdRenderTokens->raw) {
    return RawSource(source_name);
  }
  return std::nullopt;
}

// The components of a value of `data_type`: its trailing count (color3f,
// float2, texCoord2h), or 1 for a scalar (float, half, int).
std::uint32_t ComponentsOf(std::string_view data_type) {
  std::string_view name = data_type;
  while (!name.empty() &&
         std::isalpha(static_cast<unsigned char>(name.back())) != 0 &&
         name.size() > 1 &&
         std::isdigit(static_cast<unsigned char>(name.at(name.size() - 2))) !=
             0) {
    name.remove_suffix(1);
  }
  if (!name.empty() &&
      std::isdigit(static_cast<unsigned char>(name.back())) != 0) {
    const auto count = static_cast<std::uint32_t>(name.back() - '0');
    return count >= 1 && count <= 4 ? count : 1;
  }
  return 1;
}

// The channels of a var of `source` whose dataType is `data_type`.
std::uint32_t VarChannels(RenderVarSource source, std::string_view data_type) {
  const std::uint32_t channels = RenderVarOf(source).channels;
  return channels != 0 ? channels : ComponentsOf(data_type);
}

// The vars of `product` umbra renders, with the channel types the settings
// read of them, `types`, the others added to `unsupported`; the lpe and
// primvar vars, which every device leaves at 0, also to `zero`: each counted
// by its product, with its source's name.
std::vector<RenderVar> ProductVars(const Product& product,
                                   const std::vector<ChannelType>& types,
                                   Tally& unsupported, Tally& zero) {
  std::vector<RenderVar> vars;
  for (const auto& [var, type] : std::views::zip(product.renderVars, types)) {
    // A var without a sourceName is the quantity it is named after.
    const std::string name = var.varPath.GetName();
    const std::string source_name =
        var.sourceName.empty() ? name : var.sourceName;
    const auto source = SourceOf(var.sourceType, source_name);
    if (!source.has_value()) {
      unsupported.Add(product.productPath, source_name);
      continue;
    }
    const bool named = RenderVarOf(*source).kind == RenderVarKind::kNamed;
    if (named) {
      zero.Add(product.productPath, source_name);
    }
    vars.push_back({
        .name = name,
        .source = *source,
        .source_name = named ? source_name : std::string(),
        .type = type,
        .channels = VarChannels(*source, var.dataType.GetString()),
    });
  }
  return vars;
}

// "warning: usd: 3 render vars not supported (primId, first in
// /Render/beauty); left out": the render vars of `vars` (ProductVars), those
// `what`, and then `consequence`.
void ReportVars(const Tally& vars, std::string_view what,
                std::string_view consequence, Warnings& warnings) {
  const std::size_t count = vars.count();
  if (count == 0) {
    return;
  }
  warnings.Warn(std::format("{} {} {} ({}, {}in {}); {}", count,
                            Plural(count, "render var", "render vars"), what,
                            vars.detail(), Plural(count, "", "first "),
                            vars.first_path(), consequence));
}

// The default of umbra:samplesPerPixel.
constexpr std::uint32_t kDefaultSamplesPerPixel = 64;

// A product with the framing of `base` and no vars, for `file`.
RenderProduct Framed(std::string file, device::CameraId camera,
                     const SettingsBase& base) {
  return {
      .file = std::move(file),
      .camera = camera,
      .resolution = base.resolution,
      .pixel_aspect_ratio = base.pixel_aspect_ratio,
      .aspect_ratio_conform_policy =
          ConformPolicy(base.aspect_ratio_conform_policy),
      .data_window_ndc = base.data_window_ndc,
      .window = {},
      .motion_blur = base.motion_blur,
      .depth_of_field = base.depth_of_field,
      .samples_per_pixel = kDefaultSamplesPerPixel,
      .world_to_camera = {},
      .world_to_ndc = {},
      .file_pixel_aspect_ratio = base.pixel_aspect_ratio,
      .vars = {},
  };
}

// The samples per pixel `authored` on the prim at `path`: none when it
// authors none. Fails with kInvalidArgument when it is not an int above 0.
std::expected<std::optional<std::uint32_t>, Error> Authored(
    const AuthoredSamples& authored, const pxr::SdfPath& path) {
  if (authored.validity == SamplesValidity::kInvalid) {
    return std::unexpected(
        Error(ErrorCode::kInvalidArgument,
              std::format("{}: umbra:samplesPerPixel must be an int above 0",
                          path.GetString())));
  }
  return authored.samples;
}

// umbra:samplesPerPixel: that `product` authors, on the product at
// `product_path`, else that `settings` author, on the settings at
// `settings_path`, else its default.
std::expected<std::uint32_t, Error> SamplesPerPixel(
    const AuthoredSamples& product, const pxr::SdfPath& product_path,
    const AuthoredSamples& settings, const pxr::SdfPath& settings_path) {
  const auto from_product = Authored(product, product_path);
  if (!from_product.has_value()) {
    return std::unexpected(from_product.error());
  }
  if (from_product->has_value()) {
    return **from_product;
  }
  return Authored(settings, settings_path)
      .transform([](std::optional<std::uint32_t> value) {
        return value.value_or(kDefaultSamplesPerPixel);
      });
}

// Gives `product` the pixels of its data window, its file's pixel aspect
// ratio and its camera's transforms at the frame, from `camera`. Fails with
// kInvalidArgument, naming `owner`, when its data window holds no pixel.
std::expected<void, Error> Frame(RenderProduct& product,
                                 const CameraPrim& camera,
                                 std::string_view owner) {
  product.window =
      device::PixelWindowOf(product.resolution, product.data_window_ndc);
  if (product.window.IsEmpty()) {
    return std::unexpected(Error(
        ErrorCode::kInvalidArgument,
        std::format(
            "{}: its dataWindowNDC ({}, {}, {}, {}) holds no pixel "
            "of its {}x{} resolution",
            owner, product.data_window_ndc.at(0), product.data_window_ndc.at(1),
            product.data_window_ndc.at(2), product.data_window_ndc.at(3),
            product.resolution.at(0), product.resolution.at(1))));
  }
  const host::Camera at_frame = camera.AtFrame();
  const device::Camera view =
      ChangeSetBuilder::CameraOf(device::CameraId{}, at_frame);
  const device::Image image = ImageOf(product);
  const pxr::GfMatrix4d world_to_camera =
      GfMatrixOf(at_frame.camera_to_world.at(0)).GetInverse();
  const pxr::GfMatrix4d camera_to_ndc =
      GfMatrixOf(device::CameraToNdc(view, image));
  product.world_to_camera = RowMajor(world_to_camera);
  product.world_to_ndc = RowMajor(world_to_camera * camera_to_ndc);
  product.file_pixel_aspect_ratio = static_cast<float>(
      device::ConformAperture(at_frame.aperture, product.resolution,
                              static_cast<double>(product.pixel_aspect_ratio),
                              product.aspect_ratio_conform_policy)
          .pixel_aspect_ratio);
  return {};
}

}  // namespace

void ProductConverter::Report(Warnings& warnings) const {
  ReportVars(unsupported_, "not supported", "left out", warnings);
  ReportVars(zero_, "rendered as 0",
             "umbra renders only color, depth, normal and albedo", warnings);
}

std::expected<RenderProduct, Error> ProductConverter::Convert(
    const Product& product, const ProductAttributes& attributes,
    const AuthoredSamples& settings_samples) {
  const std::string owner =
      std::format("render product {}", product.productPath.GetString());
  if (product.renderVars.empty()) {
    return std::unexpected(Error(ErrorCode::kInvalidArgument,
                                 std::format("{} has no orderedVars", owner)));
  }
  const auto camera = cameras_->Index(product.cameraPath, owner);
  if (!camera.has_value()) {
    return std::unexpected(camera.error());
  }
  std::string file = ExpandFrame(product.name.GetString(), inputs_->frame);
  if (auto prepared = PrepareOutput(file); !prepared.has_value()) {
    return std::unexpected(prepared.error());
  }
  RenderProduct converted =
      Framed(std::move(file), *camera, ProductBase(product));
  converted.vars =
      ProductVars(product, attributes.var_types, unsupported_, zero_);
  if (converted.vars.empty()) {
    return std::unexpected(
        Error(ErrorCode::kInvalidArgument,
              std::format("{} has no var umbra renders", owner)));
  }
  auto samples =
      SamplesPerPixel(attributes.samples_per_pixel, product.productPath,
                      settings_samples, inputs_->settings);
  if (!samples.has_value()) {
    return std::unexpected(samples.error());
  }
  converted.samples_per_pixel = *samples;
  if (auto framed = Frame(converted, cameras_->Prim(product.cameraPath), owner);
      !framed.has_value()) {
    return std::unexpected(framed.error());
  }
  return converted;
}

std::expected<RenderProduct, Error> ProductConverter::Implicit(
    const RenderSettingsPrim& settings, device::CameraId camera) {
  std::string file = ImplicitProductFile(inputs_->scene_path, inputs_->frame);
  if (auto prepared = PrepareOutput(file); !prepared.has_value()) {
    return std::unexpected(prepared.error());
  }
  RenderProduct product = Framed(std::move(file), camera, settings.base());
  product.vars.push_back({
      .name = "color",
      .source = RenderVarSource::kColor,
      .source_name = {},
      .type = ChannelType::kHalf,
      .channels = RenderVarOf(RenderVarSource::kColor).channels,
  });
  auto samples = SamplesPerPixel(
      {}, pxr::SdfPath(), settings.samples_per_pixel(), inputs_->settings);
  if (!samples.has_value()) {
    return std::unexpected(samples.error());
  }
  product.samples_per_pixel = *samples;
  if (auto framed = Frame(
          product, cameras_->Prim(settings.CameraPath()),
          std::format("render settings {}", inputs_->settings.GetString()));
      !framed.has_value()) {
    return std::unexpected(framed.error());
  }
  return product;
}

std::expected<std::vector<RenderProduct>, Error> ProductConverter::Products(
    const RenderSettingsPrim& settings, device::CameraId camera) {
  std::vector<RenderProduct> products;
  if (settings.GetRenderProducts().empty()) {
    auto implicit = Implicit(settings, camera);
    if (!implicit.has_value()) {
      return std::unexpected(implicit.error());
    }
    products.push_back(std::move(*implicit));
    return products;
  }
  for (const auto& [product, attributes] : std::views::zip(
           settings.GetRenderProducts(), settings.product_attributes())) {
    auto converted = Convert(product, attributes, settings.samples_per_pixel());
    if (!converted.has_value()) {
      return std::unexpected(converted.error());
    }
    products.push_back(std::move(*converted));
  }
  return products;
}

}  // namespace umbra::delegate::internal
