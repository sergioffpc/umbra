// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "buffer_layout.h"

#include <algorithm>

#include "render_vars.h"
#include "umbra/delegate/render_settings.h"

namespace umbra::delegate::internal {
namespace {

// Whether `product` has a var of `source`.
bool HasVar(const RenderProduct& product, RenderVarSource source) {
  return std::ranges::contains(product.vars, source, &RenderVar::source);
}

// Whether the denoise of a product reads `var`'s, without one of its own.
bool IsMissingInput(const RenderProduct& product, const RenderVarTraits& var) {
  return var.denoise == DenoiseRole::kInput && !HasVar(product, var.source);
}

}  // namespace

BufferLayout LayoutOf(const RenderProduct& product) {
  BufferLayout layout{
      .width = product.window.width(),
      .height = product.window.height(),
      .slots = {},
  };
  layout.slots.reserve(product.vars.size() + kRenderVars.size());
  for (const RenderVar& var : product.vars) {
    layout.slots.push_back({.source = var.source, .channels = var.channels});
  }
  if (!HasVar(product, RenderVarSource::kDenoisedColor)) {
    return layout;
  }
  for (const RenderVarTraits& var : kRenderVars) {
    if (IsMissingInput(product, var)) {
      layout.slots.push_back({.source = var.source, .channels = var.channels});
    }
  }
  return layout;
}

}  // namespace umbra::delegate::internal
