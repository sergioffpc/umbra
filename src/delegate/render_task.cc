// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "render_task.h"

#include <pxr/base/gf/vec3i.h>
#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/repr.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/usd/sdf/path.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

#include "denoiser.h"
#include "hydra.h"
#include "render_pass.h"
#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/delegate/render_settings.h"
#include "umbra/delegate/session.h"

namespace umbra::delegate::internal {
namespace {

// The path of the task, which is not in the render index, and under which
// it names its render buffers.
const pxr::SdfPath& TaskPath() {
  static const pxr::SdfPath kPath("/__umbraRenderTask");
  return kPath;
}

// Whether `product` has a var of `source`.
bool HasVar(const RenderProduct& product, RenderVarSource source) {
  return std::ranges::contains(product.vars, source, &RenderVar::source);
}

// The inputs of the denoise of `product` that it has no var of: none when it
// has no denoisedColor var.
std::vector<RenderVarSource> InternalInputs(const RenderProduct& product) {
  std::vector<RenderVarSource> inputs;
  if (!HasVar(product, RenderVarSource::kDenoisedColor)) {
    return inputs;
  }
  static constexpr std::array<RenderVarSource, 3> kInputs = {
      RenderVarSource::kColor,
      RenderVarSource::kAlbedo,
      RenderVarSource::kNormal,
  };
  for (const RenderVarSource source : kInputs) {
    if (!HasVar(product, source)) {
      inputs.push_back(source);
    }
  }
  return inputs;
}

// A render buffer of `product`, the `index`th, the `slot`th of its buffers,
// with `channels` channels over its data window, set to 0, for `what`.
std::expected<std::unique_ptr<RenderBuffer>, core::Error> CreateBuffer(
    const RenderProduct& product, std::size_t index, std::size_t slot,
    std::uint32_t channels, std::string_view what) {
  auto buffer = std::make_unique<RenderBuffer>(TaskPath().AppendChild(
      pxr::TfToken(std::format("product{}_{}", index, slot))));
  if (!buffer->Allocate(
          pxr::GfVec3i(product.window.width(), product.window.height(), 1),
          FloatFormat(channels), false)) {
    return std::unexpected(
        core::Error(core::ErrorCode::kInternal,
                    std::format("{}: no render buffer of {} channels for {}",
                                product.file, channels, what)));
  }
  return buffer;
}

// The render buffers of `product`, the `index`th, sized to its data window
// and set to 0: those of its vars, then those of the inputs of its denoise
// it has no var of, RGBA for the color and RGB for the albedo and the
// normal.
std::expected<ProductBuffers, core::Error> CreateBuffers(
    const RenderProduct& product, std::size_t index) {
  const std::vector<RenderVarSource> internal = InternalInputs(product);
  ProductBuffers buffers;
  buffers.reserve(product.vars.size() + internal.size());
  for (const RenderVar& var : product.vars) {
    auto buffer =
        CreateBuffer(product, index, buffers.size(), var.channels, var.name);
    if (!buffer.has_value()) {
      return std::unexpected(std::move(buffer.error()));
    }
    buffers.push_back(*std::move(buffer));
  }
  for (const RenderVarSource source : internal) {
    const std::uint32_t channels = source == RenderVarSource::kColor ? 4 : 3;
    auto buffer = CreateBuffer(product, index, buffers.size(), channels,
                               AovName(source).GetString());
    if (!buffer.has_value()) {
      return std::unexpected(std::move(buffer.error()));
    }
    buffers.push_back(*std::move(buffer));
  }
  return buffers;
}

// The AOV binding of `buffer` as the AOV of `source`.
pxr::HdRenderPassAovBinding BindingOf(RenderVarSource source,
                                      RenderBuffer& buffer) {
  pxr::HdRenderPassAovBinding binding;
  binding.aovName = AovName(source);
  binding.renderBuffer = &buffer;
  binding.renderBufferId = buffer.GetId();
  return binding;
}

// The AOV bindings of `buffers`, those CreateBuffers gives `product`, by the
// names AovName gives their sources.
pxr::HdRenderPassAovBindingVector BindingsOf(const RenderProduct& product,
                                             const ProductBuffers& buffers) {
  pxr::HdRenderPassAovBindingVector bindings;
  bindings.reserve(buffers.size());
  for (const auto& [var, buffer] : std::views::zip(product.vars, buffers)) {
    bindings.push_back(BindingOf(var.source, *buffer));
  }
  for (const auto& [source, buffer] :
       std::views::zip(InternalInputs(product),
                       buffers | std::views::drop(product.vars.size()))) {
    bindings.push_back(BindingOf(source, *buffer));
  }
  return bindings;
}

}  // namespace

RenderTask::RenderTask(RenderDelegate& delegate, pxr::SdfPath settings,
                       DenoiseDevice denoise_device,
                       Display* UMBRA_NULLABLE display)
    : pxr::HdTask(TaskPath()),
      delegate_(&delegate),
      settings_(std::move(settings)),
      denoise_device_(std::move(denoise_device)),
      display_(display) {}

void RenderTask::SetOnCommitted(std::function<void()> on_committed) {
  on_committed_ = std::move(on_committed);
}

void RenderTask::Sync(pxr::HdSceneDelegate* UMBRA_NULLABLE /*delegate*/,
                      pxr::HdTaskContext* /*context*/,
                      pxr::HdDirtyBits* dirty_bits) {
  // The sprims (the render settings, the cameras, the lights) are synced
  // before the tasks, and the rprims after.
  render_tags_ = delegate_->RenderTags(settings_);
  delegate_->ChooseShutter(settings_);
  delegate_->ChooseDicing(settings_);
  delegate_->Place(render_tags_);
  *dirty_bits = pxr::HdChangeTracker::Clean;
}

void RenderTask::Prepare(pxr::HdTaskContext* /*context*/,
                         pxr::HdRenderIndex* index) {
  index_ = index;
}

void RenderTask::Execute(pxr::HdTaskContext* /*context*/) {
  // CommitResources ran between the syncs and the execution of the tasks.
  const Committed* const UMBRA_NULLABLE committed = delegate_->PeekCommitted();
  if (committed == nullptr) {
    result_ = std::nullopt;
    return;
  }
  if (on_committed_) {
    on_committed_();
  }
  result_ = Render(*committed);
}

const pxr::TfTokenVector& RenderTask::GetRenderTags() const {
  return render_tags_;
}

std::optional<std::expected<void, core::Error>> RenderTask::TakeResult() {
  return std::exchange(result_, std::nullopt);
}

std::expected<Denoiser * UMBRA_NULLABLE, core::Error> RenderTask::DenoiserOf(
    const RenderProduct& product) {
  if (!HasVar(product, RenderVarSource::kDenoisedColor)) {
    return nullptr;
  }
  if (denoiser_ == nullptr) {
    auto created = Denoiser::Create(denoise_device_);
    if (!created.has_value()) {
      return std::unexpected(std::move(created.error()));
    }
    denoiser_ = *std::move(created);
  }
  return denoiser_.get();
}

std::expected<void, core::Error> RenderTask::Render(
    const Committed& committed) {
  if (index_ == nullptr) {
    return std::unexpected(core::Error(core::ErrorCode::kInternal,
                                       "the render task was not prepared"));
  }
  pxr::HdRenderIndex& render_index = *index_;
  const std::vector<RenderProduct>& products = committed.settings.products;
  buffers_.resize(products.size());
  while (passes_.size() < products.size()) {
    passes_.push_back(
        std::static_pointer_cast<RenderPass>(delegate_->CreateRenderPass(
            &render_index, pxr::HdRprimCollection(
                               pxr::HdTokens->geometry,
                               pxr::HdReprSelector(pxr::HdReprTokens->hull)))));
  }
  for (std::size_t index = 0; index < products.size(); ++index) {
    if (auto rendered = RenderOne(committed, index); !rendered.has_value()) {
      return rendered;
    }
  }
  return {};
}

std::expected<void, core::Error> RenderTask::RenderOne(
    const Committed& committed, std::size_t index) {
  const RenderProduct& product = committed.settings.products.at(index);
  auto buffers = CreateBuffers(product, index);
  if (!buffers.has_value()) {
    return std::unexpected(std::move(buffers.error()));
  }
  const auto denoiser = DenoiserOf(product);
  if (!denoiser.has_value()) {
    return std::unexpected(denoiser.error());
  }
  buffers_.at(index) = *std::move(buffers);
  const auto state = std::make_shared<pxr::HdRenderPassState>();
  state->SetAovBindings(BindingsOf(product, buffers_.at(index)));
  RenderPass& pass = *passes_.at(index);
  pass.SetProduct(product, committed.frame, *denoiser, display_);
  pass.Execute(state, render_tags_);
  return pass.TakeResult();
}

}  // namespace umbra::delegate::internal
