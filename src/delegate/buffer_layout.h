// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The layout of the render buffers a product renders into: what the render
// task keeps them across frames by, as long as it does not change.

#ifndef UMBRA_DELEGATE_BUFFER_LAYOUT_H_
#define UMBRA_DELEGATE_BUFFER_LAYOUT_H_

#include <cstdint>
#include <vector>

#include "umbra/core/safety.h"
#include "umbra/delegate/render_settings.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::delegate::internal {

// One render buffer of a product: the AOV of `source` it binds, with
// `channels` float32 channels per pixel.
struct BufferSlot {
  RenderVarSource source = RenderVarSource::kColor;
  std::uint32_t channels = 1;

  friend bool operator==(const BufferSlot&, const BufferSlot&) = default;
};

// The render buffers of a product, each `width` by `height` pixels, its data
// window.
struct BufferLayout {
  std::int32_t width = 0;
  std::int32_t height = 0;
  std::vector<BufferSlot> slots;

  friend bool operator==(const BufferLayout&, const BufferLayout&) = default;
};

// The render buffers of `product`: one per var, in its order, then, for a
// product with a denoisedColor var, one per input of its denoise it has no
// var of, in the order of kRenderVars (color, normal, albedo), with the
// channels of its source.
[[nodiscard]] BufferLayout LayoutOf(const RenderProduct& product);

}  // namespace umbra::delegate::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DELEGATE_BUFFER_LAYOUT_H_
