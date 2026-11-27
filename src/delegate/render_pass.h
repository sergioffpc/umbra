// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// umbra's render pass and render buffers in Hydra: one pass per render
// product, which runs every device on a host thread of its own over the
// product's tiles (docs/adr/0014-frames-render-in-tiles-from-one-counter.md),
// then denoises the product's color when it asks for it, and the render
// buffers it fills, float32 over the product's data window, created by the
// delegate rather than as bprims.

#ifndef UMBRA_DELEGATE_RENDER_PASS_H_
#define UMBRA_DELEGATE_RENDER_PASS_H_

#include <pxr/base/gf/vec3i.h>
#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include "denoiser.h"
#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/delegate/render_settings.h"
#include "umbra/delegate/session.h"
#include "umbra/device/device.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::delegate::internal {

namespace pxr = PXR_NS;

// The render buffer of one render var of a product: float32, with 1 to 4
// channels per pixel, over the product's data window, the pixels row by row
// from its top left. It lives in host memory, and is always mapped.
class RenderBuffer final : public pxr::HdRenderBuffer {
 public:
  explicit RenderBuffer(const pxr::SdfPath& buffer_id);

  // Allocates `dimensions` (width, height, 1) pixels of `format`, a float32
  // format, and sets them to 0. Fails for another format, a depth other
  // than 1, or multisampling.
  bool Allocate(const pxr::GfVec3i& dimensions, pxr::HdFormat format,
                // NOLINTNEXTLINE(umbra-bool-parameter): Hydra's interface.
                bool multi_sampled) override;
  [[nodiscard]] unsigned int GetWidth() const override;
  [[nodiscard]] unsigned int GetHeight() const override;
  [[nodiscard]] unsigned int GetDepth() const override;
  [[nodiscard]] pxr::HdFormat GetFormat() const override;
  [[nodiscard]] bool IsMultiSampled() const override;
  void* Map() override;
  void Unmap() override;
  [[nodiscard]] bool IsMapped() const override;
  void Resolve() override;
  [[nodiscard]] bool IsConverged() const override;

  // Its values.
  [[nodiscard]] std::span<float> pixels() UMBRA_LIFETIMEBOUND {
    return pixels_;
  }
  [[nodiscard]] std::span<const float> pixels() const UMBRA_LIFETIMEBOUND {
    return pixels_;
  }
  // The values per pixel.
  [[nodiscard]] std::uint32_t channels() const { return channels_; }

 protected:
  // NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp): the
  // name of the method OpenUSD has render buffers override.
  void _Deallocate() override;
  // NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)

 private:
  unsigned int width_ = 0;
  unsigned int height_ = 0;
  std::uint32_t channels_ = 0;
  pxr::HdFormat format_ = pxr::HdFormatInvalid;
  std::vector<float> pixels_;
};

// The render buffers a denoise reads, those of the "color", "albedo" and
// "normal" AOVs, and those it writes, of the "denoisedColor" AOVs.
struct DenoiseBuffers {
  const RenderBuffer* UMBRA_NULLABLE color = nullptr;
  const RenderBuffer* UMBRA_NULLABLE albedo = nullptr;
  const RenderBuffer* UMBRA_NULLABLE normal = nullptr;
  std::vector<RenderBuffer*> outputs;
};

// The float32 format of `channels` channels, 1 to 4.
[[nodiscard]] pxr::HdFormat FloatFormat(std::uint32_t channels);

// The name of the AOV of a var of `source`, which binds its render buffer:
// "color", "depth", "normal", "albedo"; "denoisedColor" for those the pass
// denoises, and "lpe" and "primvar" for those that no device renders.
[[nodiscard]] pxr::TfToken AovName(RenderVarSource source);

// The render pass of one render product. Each Execute renders the product
// SetProduct gave it, on every device at once, into the render buffers bound
// to the pass state's AOVs, those of AOVs no device renders left at 0, as a
// phase of the render, "Rendering <file>", whose tiles the devices count.
// When a "denoisedColor" AOV is bound, it then denoises the buffer of the
// "color" AOV into it, with those of "albedo" and "normal", on the whole
// image in host memory, as a phase of its own, "Denoising <file>", which
// counts its pixels. A display, if SetProduct gives one, shows the product
// as its tiles finish, from a host thread of its own, and once more whole
// when it has been denoised.
class RenderPass final : public pxr::HdRenderPass {
 public:
  // Renders on `devices`, which outlive it.
  RenderPass(pxr::HdRenderIndex* index,
             const pxr::HdRprimCollection& collection,
             std::span<device::Device* const> devices UMBRA_LIFETIMEBOUND);

  // The product the next Execute renders, with the frame parameters of the
  // frame, which give the shutter, the denoiser of its denoisedColor vars,
  // if it has any, and the display that shows it, if any; all outlive that
  // Execute.
  void SetProduct(
      const RenderProduct& product UMBRA_LIFETIME_CAPTURE_BY_THIS,
      const device::FrameParameters& frame UMBRA_LIFETIME_CAPTURE_BY_THIS,
      Denoiser* UMBRA_NULLABLE denoiser UMBRA_LIFETIME_CAPTURE_BY_THIS,
      Display* UMBRA_NULLABLE display UMBRA_LIFETIME_CAPTURE_BY_THIS);

  // What the last Execute gave: fails with the first device's error, in
  // order, naming it, with kUnavailable when no device took the tiles, or
  // with the denoiser's error.
  [[nodiscard]] std::expected<void, core::Error> TakeResult();

 protected:
  // NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp): the
  // name of the method OpenUSD has render passes override.
  void _Execute(const pxr::HdRenderPassStateSharedPtr& state,
                const pxr::TfTokenVector& render_tags) override;
  // NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)

 private:
  // Renders the product on every device, into `vars`, and shows `shown`,
  // every var's buffer, on the display.
  [[nodiscard]] std::expected<void, core::Error> Render(
      std::span<const device::FilmVar> vars,
      std::span<const RenderedVar> shown);
  // Denoises the product's color into `buffers`' outputs, if any.
  [[nodiscard]] std::expected<void, core::Error> Denoise(
      const DenoiseBuffers& buffers);

  std::span<device::Device* const> devices_;
  const RenderProduct* UMBRA_NULLABLE product_ = nullptr;
  const device::FrameParameters* UMBRA_NULLABLE frame_ = nullptr;
  Denoiser* UMBRA_NULLABLE denoiser_ = nullptr;
  Display* UMBRA_NULLABLE display_ = nullptr;
  std::optional<std::expected<void, core::Error>> result_;
};

}  // namespace umbra::delegate::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DELEGATE_RENDER_PASS_H_
