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

#include "buffer_layout.h"
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

// The render buffers of `product`, the `index`th, those of `layout`, its
// layout, set to 0: those of its vars, named after them, then those of the
// inputs of its denoise it has no var of, named after their AOVs.
std::expected<ProductBuffers, core::Error> CreateBuffers(
    const RenderProduct& product, const BufferLayout& layout,
    std::size_t index) {
  ProductBuffers buffers;
  buffers.reserve(layout.slots.size());
  for (const BufferSlot& slot : layout.slots) {
    const std::size_t position = buffers.size();
    auto buffer = CreateBuffer(product, index, position, slot.channels,
                               position < product.vars.size()
                                   ? product.vars.at(position).name
                                   : AovName(slot.source).GetString());
    if (!buffer.has_value()) {
      return std::unexpected(std::move(buffer.error()));
    }
    buffers.push_back(*std::move(buffer));
  }
  return buffers;
}

// The AOV bindings of `buffers`, those CreateBuffers gives `layout`, by the
// names AovName gives their sources.
pxr::HdRenderPassAovBindingVector BindingsOf(const BufferLayout& layout,
                                             const ProductBuffers& buffers) {
  pxr::HdRenderPassAovBindingVector bindings;
  bindings.reserve(buffers.size());
  for (const auto& [slot, buffer] : std::views::zip(layout.slots, buffers)) {
    pxr::HdRenderPassAovBinding& binding = bindings.emplace_back();
    binding.aovName = AovName(slot.source);
    binding.renderBuffer = buffer.get();
    binding.renderBufferId = buffer->GetId();
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
  layouts_.resize(products.size());
  states_.resize(products.size());
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

std::expected<void, core::Error> RenderTask::Bind(const RenderProduct& product,
                                                  std::size_t index) {
  BufferLayout layout = LayoutOf(product);
  std::shared_ptr<pxr::HdRenderPassState>& state = states_.at(index);
  if (state != nullptr && layout == layouts_.at(index)) {
    return {};
  }
  auto buffers = CreateBuffers(product, layout, index);
  if (!buffers.has_value()) {
    return std::unexpected(std::move(buffers.error()));
  }
  buffers_.at(index) = *std::move(buffers);
  layouts_.at(index) = std::move(layout);
  state = std::make_shared<pxr::HdRenderPassState>();
  state->SetAovBindings(BindingsOf(layouts_.at(index), buffers_.at(index)));
  return {};
}

std::expected<void, core::Error> RenderTask::RenderOne(
    const Committed& committed, std::size_t index) {
  const RenderProduct& product = committed.settings.products.at(index);
  if (auto bound = Bind(product, index); !bound.has_value()) {
    return bound;
  }
  const auto denoiser = DenoiserOf(product);
  if (!denoiser.has_value()) {
    return std::unexpected(denoiser.error());
  }
  RenderPass& pass = *passes_.at(index);
  pass.SetProduct(product, committed.frame, *denoiser, display_);
  pass.Execute(states_.at(index), render_tags_);
  return pass.TakeResult();
}

}  // namespace umbra::delegate::internal
