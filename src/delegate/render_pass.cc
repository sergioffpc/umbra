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

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "denoiser.h"
#include "device_threads.h"
#include "umbra/core/error.h"
#include "umbra/core/floating_point.h"
#include "umbra/core/progress.h"
#include "umbra/core/safety.h"
#include "umbra/delegate/render_settings.h"
#include "umbra/delegate/session.h"
#include "umbra/device/device.h"
#include "umbra/device/image.h"

namespace umbra::delegate::internal {
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

// How often a display looks for the tiles finished since it last looked.
constexpr std::chrono::milliseconds kDisplayInterval{50};

// Shows on `display` the tiles of `tiles` finished since `shown`, which
// marks those it showed.
void ShowFinished(Display& display, const device::TileQueue& tiles,
                  std::vector<std::uint8_t>& shown) {
  for (std::uint64_t tile = 0; tile < tiles.count(); ++tile) {
    std::uint8_t& done = shown.at(static_cast<std::size_t>(tile));
    if (done == 0 && tiles.IsFinished(tile)) {
      display.Update(tiles.TileWindow(tile));
      done = 1;
    }
  }
}

// Shows `product`, rendering into `vars` from `tiles`, on `display` as its
// tiles finish, until `stop` is requested; then shows the tiles finished
// last. The render pass ends the display.
void Feed(const std::stop_token& stop, Display& display,
          const RenderProduct& product, std::span<const RenderedVar> vars,
          const device::TileQueue& tiles) {
  core::ConfigureThreadFloatingPoint();
  display.Begin(product, vars);
  std::vector<std::uint8_t> shown(static_cast<std::size_t>(tiles.count()));
  // An interruptible sleep: it wakes when the interval passes, or at once
  // when the devices are done.
  std::mutex mutex;
  std::condition_variable_any wake;
  while (!stop.stop_requested()) {
    ShowFinished(display, tiles, shown);
    std::unique_lock lock(mutex);
    wake.wait_for(lock, stop, kDisplayInterval, [] { return false; });
  }
  ShowFinished(display, tiles, shown);
}

// The render buffers a denoise reads and writes among `bindings`: the first
// of each input AOV, and every denoisedColor AOV.
DenoiseBuffers DenoiseBuffersOf(
    const pxr::HdRenderPassAovBindingVector& bindings) {
  static const pxr::TfToken kColor = AovName(RenderVarSource::kColor);
  static const pxr::TfToken kAlbedo = AovName(RenderVarSource::kAlbedo);
  static const pxr::TfToken kNormal = AovName(RenderVarSource::kNormal);
  static const pxr::TfToken kDenoised =
      AovName(RenderVarSource::kDenoisedColor);
  DenoiseBuffers buffers;
  for (const pxr::HdRenderPassAovBinding& binding : bindings) {
    auto* const buffer = dynamic_cast<RenderBuffer*>(binding.renderBuffer);
    if (buffer == nullptr) {
      continue;
    }
    const pxr::TfToken& name = binding.aovName;
    if (name == kDenoised) {
      buffers.outputs.push_back(buffer);
    } else if (name == kColor && buffers.color == nullptr) {
      buffers.color = buffer;
    } else if (name == kAlbedo && buffers.albedo == nullptr) {
      buffers.albedo = buffer;
    } else if (name == kNormal && buffers.normal == nullptr) {
      buffers.normal = buffer;
    }
  }
  return buffers;
}

// The vars a product's devices render, and those a display shows.
struct BoundVars {
  std::vector<device::FilmVar> rendered;
  std::vector<RenderedVar> shown;
};

// The vars of `product` bound by `bindings`: the first bindings are those of
// its vars, in its order; those after them, the inputs of its denoise, no
// file holds, and no display shows.
BoundVars BoundVarsOf(const RenderProduct& product,
                      const pxr::HdRenderPassAovBindingVector& bindings) {
  BoundVars bound;
  for (std::size_t index = 0; index < bindings.size(); ++index) {
    const pxr::HdRenderPassAovBinding& binding = bindings.at(index);
    auto* const buffer = dynamic_cast<RenderBuffer*>(binding.renderBuffer);
    if (buffer == nullptr) {
      continue;
    }
    if (index < product.vars.size()) {
      bound.shown.push_back(
          {.var = &product.vars.at(index), .pixels = buffer->pixels()});
    }
    if (const std::optional<device::VarSource> source =
            SourceOf(binding.aovName)) {
      bound.rendered.push_back({
          .source = *source,
          .pixels = buffer->pixels(),
          .channels = buffer->channels(),
      });
    }
  }
  return bound;
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
  static const std::array<pxr::TfToken, 7> kNames = {
      pxr::TfToken("color"),   pxr::TfToken("denoisedColor"),
      pxr::TfToken("depth"),   pxr::TfToken("normal"),
      pxr::TfToken("albedo"),  pxr::TfToken("lpe"),
      pxr::TfToken("primvar"),
  };
  return kNames.at(static_cast<std::size_t>(source));
}

RenderPass::RenderPass(pxr::HdRenderIndex* index,
                       const pxr::HdRprimCollection& collection,
                       std::span<device::Device* const> devices)
    : pxr::HdRenderPass(index, collection), devices_(devices) {}

void RenderPass::SetProduct(const RenderProduct& product,
                            const device::FrameParameters& frame,
                            Denoiser* UMBRA_NULLABLE denoiser,
                            Display* UMBRA_NULLABLE display) {
  product_ = &product;
  frame_ = &frame;
  denoiser_ = denoiser;
  display_ = display;
}

std::expected<void, Error> RenderPass::TakeResult() {
  return std::exchange(result_, std::nullopt)
      .value_or(std::unexpected(
          Error(ErrorCode::kInternal, "the render pass did not run")));
}

void RenderPass::_Execute(const pxr::HdRenderPassStateSharedPtr& state,
                          const pxr::TfTokenVector& /*render_tags*/) {
  if (product_ == nullptr) {
    result_ = std::unexpected(
        Error(ErrorCode::kInternal, "the render pass has no product"));
    return;
  }
  const pxr::HdRenderPassAovBindingVector& bindings = state->GetAovBindings();
  const BoundVars bound = BoundVarsOf(*product_, bindings);
  const DenoiseBuffers denoise = DenoiseBuffersOf(bindings);
  result_ = Render(bound.rendered, bound.shown).and_then([&] {
    return Denoise(denoise);
  });
  if (display_ != nullptr) {
    if (result_->has_value() && !denoise.outputs.empty()) {
      display_->Update(product_->window);
    }
    display_->End();
  }
}

std::expected<void, Error> RenderPass::Render(
    std::span<const device::FilmVar> vars, std::span<const RenderedVar> shown) {
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
  // Shows the tiles until the devices are done, then ends.
  std::optional<std::jthread> feed;
  if (display_ != nullptr) {
    feed.emplace(Feed, std::ref(*display_), std::cref(product), shown,
                 std::cref(tiles));
  }
  auto rendered =
      OnEachDevice(devices_, [&frame, &tiles](device::Device& device) {
        return device.Render(frame, tiles);
      });
  feed.reset();
  if (!rendered.has_value()) {
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

std::expected<void, Error> RenderPass::Denoise(const DenoiseBuffers& buffers) {
  if (buffers.outputs.empty()) {
    return {};
  }
  if (product_ == nullptr || denoiser_ == nullptr || buffers.color == nullptr ||
      buffers.albedo == nullptr || buffers.normal == nullptr) {
    return std::unexpected(
        Error(ErrorCode::kInternal,
              "the render pass has no denoiser or no buffer to denoise"));
  }
  const RenderProduct& product = *product_;
  const std::int32_t width = product.window.width();
  const std::int32_t height = product.window.height();
  // The phase ends, failed, before the error is reported.
  core::ProgressPhase phase(
      std::format("Denoising {}", product.file), "pixels",
      static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height));
  RenderBuffer& output = *buffers.outputs.front();
  if (auto denoised = denoiser_->Denoise(
          {
              .width = width,
              .height = height,
              .color = buffers.color->pixels(),
              .albedo = buffers.albedo->pixels(),
              .normal = buffers.normal->pixels(),
              .output = output.pixels(),
          },
          phase);
      !denoised.has_value()) {
    return std::unexpected(
        Error(denoised.error().code(),
              std::format("{}: {}", product.file, denoised.error().message())));
  }
  for (RenderBuffer* const other : buffers.outputs | std::views::drop(1)) {
    std::ranges::copy(output.pixels(), other->pixels().begin());
  }
  phase.Finish();
  return {};
}

}  // namespace umbra::delegate::internal
