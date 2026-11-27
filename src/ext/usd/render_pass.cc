// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "render_pass.h"

#include <pxr/base/gf/vec3i.h>
#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/usd/sdf/path.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "umbra/core/error.h"
#include "umbra/core/progress.h"
#include "umbra/device/device.h"
#include "umbra/ext/usd/render_settings.h"

namespace umbra::ext::usd::internal {
namespace {

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;

// The source a device renders for the AOV `name`, if any.
std::optional<device::VarSource> SourceOf(const pxr::TfToken& name) {
  static const std::array<std::pair<pxr::TfToken, device::VarSource>, 4>
      kSources = {
          {
              {AovName(RenderVarSource::kColor), device::VarSource::kColor},
              {AovName(RenderVarSource::kDepth), device::VarSource::kDepth},
              {AovName(RenderVarSource::kNormal), device::VarSource::kNormal},
              {AovName(RenderVarSource::kAlbedo), device::VarSource::kAlbedo},
          },
  };
  for (const auto& [aov, source] : kSources) {
    if (aov == name) {
      return source;
    }
  }
  return std::nullopt;
}

// The frame parameters of `product`, with the shutter of `frame`, rendering
// `vars`.
device::FrameParameters FrameOf(const RenderProduct& product,
                                const device::FrameParameters& frame,
                                std::span<const device::FilmVar> vars) {
  return {
      .shutter = frame.shutter,
      .camera = product.camera,
      .image =
          {
              .resolution = product.resolution,
              .window = product.window,
              .pixel_aspect_ratio = product.pixel_aspect_ratio,
              .aspect_ratio_conform_policy =
                  product.aspect_ratio_conform_policy,
          },
      .motion_blur = product.motion_blur,
      .depth_of_field = product.depth_of_field,
      .samples_per_pixel = product.samples_per_pixel,
      .vars = vars,
  };
}

// Renders `frame` on every device of `devices`, each on a host thread of its
// own, from `tiles`: the first error, in their order, naming its device.
std::expected<void, Error> RenderOnEach(
    std::span<device::Device* const> devices,
    const device::FrameParameters& frame, device::TileQueue& tiles) {
  std::vector<std::expected<void, Error>> results(devices.size());
  {
    const auto render = [&frame, &tiles](device::Device* device,
                                         std::expected<void, Error>& result) {
      result = device->Render(frame, tiles);
    };
    std::vector<std::jthread> threads;
    threads.reserve(devices.size());
    for (const auto& [device, result] : std::views::zip(devices, results)) {
      threads.emplace_back(render, device, std::ref(result));
    }
  }
  for (const auto& [device, result] : std::views::zip(devices, results)) {
    if (!result.has_value()) {
      return std::unexpected(Error(result.error().code(),
                                   std::format("device {}: {}", device->name(),
                                               result.error().message())));
    }
  }
  return {};
}

// "cpu0, cpu1".
std::string Names(std::span<device::Device* const> devices) {
  std::string names;
  for (const device::Device* device : devices) {
    names += names.empty() ? std::string(device->name())
                           : std::format(", {}", device->name());
  }
  return names;
}

}  // namespace

RenderBuffer::RenderBuffer(const pxr::SdfPath& buffer_id)
    : pxr::HdRenderBuffer(buffer_id) {}

bool RenderBuffer::Allocate(
    const pxr::GfVec3i& dimensions, pxr::HdFormat format,
    // NOLINTNEXTLINE(umbra-bool-parameter): Hydra's interface.
    bool multi_sampled) {
  const std::size_t components = pxr::HdGetComponentCount(format);
  const int width = dimensions.x;
  const int height = dimensions.y;
  if (multi_sampled || dimensions.z != 1 || width < 0 || height < 0 ||
      FloatFormat(static_cast<std::uint32_t>(components)) != format) {
    return false;
  }
  width_ = static_cast<unsigned int>(width);
  height_ = static_cast<unsigned int>(height);
  channels_ = static_cast<std::uint32_t>(components);
  format_ = format;
  pixels_.assign(static_cast<std::size_t>(width_) * height_ * channels_, 0.0F);
  return true;
}

unsigned int RenderBuffer::GetWidth() const { return width_; }

unsigned int RenderBuffer::GetHeight() const { return height_; }

unsigned int RenderBuffer::GetDepth() const { return 1; }

pxr::HdFormat RenderBuffer::GetFormat() const { return format_; }

bool RenderBuffer::IsMultiSampled() const { return false; }

void* RenderBuffer::Map() { return pixels_.data(); }

void RenderBuffer::Unmap() {}

bool RenderBuffer::IsMapped() const { return true; }

void RenderBuffer::Resolve() {}

bool RenderBuffer::IsConverged() const { return true; }

void RenderBuffer::_Deallocate() {
  pixels_ = {};
  width_ = 0;
  height_ = 0;
  channels_ = 0;
  format_ = pxr::HdFormatInvalid;
}

pxr::HdFormat FloatFormat(std::uint32_t channels) {
  switch (channels) {
    case 1:
      return pxr::HdFormatFloat32;
    case 2:
      return pxr::HdFormatFloat32Vec2;
    case 3:
      return pxr::HdFormatFloat32Vec3;
    case 4:
      return pxr::HdFormatFloat32Vec4;
    default:
      break;
  }
  return pxr::HdFormatInvalid;
}

pxr::TfToken AovName(RenderVarSource source) {
  static const std::array<pxr::TfToken, 6> kNames = {
      pxr::TfToken("color"),  pxr::TfToken("depth"), pxr::TfToken("normal"),
      pxr::TfToken("albedo"), pxr::TfToken("lpe"),   pxr::TfToken("primvar"),
  };
  return kNames.at(static_cast<std::size_t>(source));
}

RenderPass::RenderPass(pxr::HdRenderIndex* index,
                       const pxr::HdRprimCollection& collection,
                       std::span<device::Device* const> devices)
    : pxr::HdRenderPass(index, collection), devices_(devices) {}

void RenderPass::SetProduct(const RenderProduct& product,
                            const device::FrameParameters& frame) {
  product_ = &product;
  frame_ = &frame;
}

std::expected<void, Error> RenderPass::TakeResult() {
  return std::exchange(result_, std::nullopt)
      .value_or(std::unexpected(
          Error(ErrorCode::kInternal, "the render pass did not run")));
}

void RenderPass::_Execute(const pxr::HdRenderPassStateSharedPtr& state,
                          const pxr::TfTokenVector& /*render_tags*/) {
  std::vector<device::FilmVar> vars;
  for (const pxr::HdRenderPassAovBinding& binding : state->GetAovBindings()) {
    auto* const buffer = dynamic_cast<RenderBuffer*>(binding.renderBuffer);
    const std::optional<device::VarSource> source = SourceOf(binding.aovName);
    if (buffer != nullptr && source.has_value()) {
      vars.push_back({
          .source = *source,
          .pixels = buffer->pixels(),
          .channels = buffer->channels(),
      });
    }
  }
  result_ = Render(vars);
}

std::expected<void, Error> RenderPass::Render(
    std::span<const device::FilmVar> vars) {
  if (product_ == nullptr || frame_ == nullptr) {
    return std::unexpected(
        Error(ErrorCode::kInternal, "the render pass has no product"));
  }
  const RenderProduct& product = *product_;
  const device::FrameParameters frame = FrameOf(product, *frame_, vars);
  // The phase ends, failed, before the error is reported.
  core::ProgressPhase phase(std::format("Rendering {}", product.file), "tiles",
                            core::ProgressRate::kShown);
  device::TileQueue tiles(product.window, &phase);
  phase.SetTotal(tiles.count());
  if (auto rendered = RenderOnEach(devices_, frame, tiles);
      !rendered.has_value()) {
    return rendered;
  }
  if (tiles.taken() < tiles.count()) {
    return std::unexpected(Error(
        ErrorCode::kUnavailable,
        std::format("no device rendered '{}': {} {} no tiles", product.file,
                    Names(devices_), devices_.size() == 1 ? "takes" : "take")));
  }
  phase.Finish();
  return {};
}

}  // namespace umbra::ext::usd::internal
