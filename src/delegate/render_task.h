// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The task of umbra's that runs in HdEngine::Execute, and the render
// buffers it renders each product into.

#ifndef UMBRA_DELEGATE_RENDER_TASK_H_
#define UMBRA_DELEGATE_RENDER_TASK_H_

#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "buffer_layout.h"
#include "denoiser.h"
#include "hydra.h"
#include "render_pass.h"
#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/delegate/render_settings.h"
#include "umbra/delegate/session.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::delegate::internal {

namespace pxr = PXR_NS;

// The render buffers of a rendered product: one per var, in its order, then
// one per input of its denoise (color, normal, albedo) it has no var of,
// which no file holds.
using ProductBuffers = std::vector<std::unique_ptr<RenderBuffer>>;

// The task of umbra's that runs in HdEngine::Execute. It syncs the rprims of
// the purposes the settings include, over the shutter it chooses. Once the
// delegate has committed, it calls the callback it is given, then renders
// each product of the settings committed through a render pass of its own,
// into render buffers it creates, sized to the product's data window: one
// per var, whose AOV is AovName of its source, and, for a product with a
// denoisedColor var, one per input of the denoise it has no var of. A
// product's buffers, and the render pass state that binds them, are kept
// across frames while its buffer layout (LayoutOf) does not change, holding
// the last frame's pixels until the devices render over them; they are
// created again, set to 0, when it does. The denoiser of the denoisedColor
// vars is created for the first product that has one, and serves every
// later product and frame.
class RenderTask final : public pxr::HdTask {
 public:
  // Syncs the prims of the render index whose render delegate is
  // `delegate`, denoises on `denoise_device`, and shows the products on
  // `display`, if any, which outlives it.
  RenderTask(RenderDelegate& delegate UMBRA_LIFETIMEBOUND,
             pxr::SdfPath settings, DenoiseDevice denoise_device,
             Display* UMBRA_NULLABLE display UMBRA_LIFETIMEBOUND);

  // What Execute calls once the delegate has committed, before it renders.
  void SetOnCommitted(std::function<void()> on_committed);

  void Sync(pxr::HdSceneDelegate* UMBRA_NULLABLE delegate,
            pxr::HdTaskContext* context, pxr::HdDirtyBits* dirty_bits) override;
  void Prepare(pxr::HdTaskContext* context, pxr::HdRenderIndex* index) override;
  void Execute(pxr::HdTaskContext* context) override;
  [[nodiscard]] const pxr::TfTokenVector& GetRenderTags() const override;

  // What the last Execute rendered: fails with the error of the product that
  // failed, which stops the render. None when the commit failed, or before
  // Execute.
  [[nodiscard]] std::optional<std::expected<void, core::Error>> TakeResult();

  // The render buffers of each product rendered last, in the settings'
  // order.
  [[nodiscard]] std::span<const ProductBuffers> buffers() const
      UMBRA_LIFETIMEBOUND {
    return buffers_;
  }

 private:
  // Renders each product of `committed`.
  [[nodiscard]] std::expected<void, core::Error> Render(
      const Committed& committed);
  // Binds the render buffers of `product`, the `index`th, to its render pass
  // state: those it has, while their layout fits it, or new ones.
  [[nodiscard]] std::expected<void, core::Error> Bind(
      const RenderProduct& product, std::size_t index);
  // Renders the `index`th product of `committed`, once the passes are
  // created.
  [[nodiscard]] std::expected<void, core::Error> RenderOne(
      const Committed& committed, std::size_t index);
  // The denoiser of `product`, created if it is not yet: null when it has
  // no denoisedColor var.
  [[nodiscard]] std::expected<Denoiser * UMBRA_NULLABLE, core::Error>
  DenoiserOf(const RenderProduct& product);

  RenderDelegate* delegate_;
  pxr::SdfPath settings_;
  DenoiseDevice denoise_device_;
  std::unique_ptr<Denoiser> denoiser_;
  pxr::TfTokenVector render_tags_;
  pxr::HdRenderIndex* UMBRA_NULLABLE index_ = nullptr;
  std::function<void()> on_committed_;
  Display* UMBRA_NULLABLE display_;
  std::vector<std::shared_ptr<RenderPass>> passes_;
  std::vector<ProductBuffers> buffers_;
  // The layout of each product's buffers, and the render pass state that
  // binds them, null until they are created.
  std::vector<BufferLayout> layouts_;
  std::vector<std::shared_ptr<pxr::HdRenderPassState>> states_;
  std::optional<std::expected<void, core::Error>> result_;
};

}  // namespace umbra::delegate::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DELEGATE_RENDER_TASK_H_
