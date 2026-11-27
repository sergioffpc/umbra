// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "channels.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "umbra/ext/usd/render_settings.h"

namespace umbra {

std::span<const std::string_view> ChannelNames(ext::usd::RenderVarSource source,
                                               std::uint32_t channels) {
  using ext::usd::RenderVarSource;
  static constexpr std::array<std::string_view, 4> kColor{"R", "G", "B", "A"};
  static constexpr std::array<std::string_view, 1> kDepth{"Z"};
  static constexpr std::array<std::string_view, 4> kVector{"X", "Y", "Z", "W"};
  std::span<const std::string_view> names = kVector;
  switch (source) {
    case RenderVarSource::kColor:
    case RenderVarSource::kDenoisedColor:
    case RenderVarSource::kAlbedo:
    case RenderVarSource::kLpe:
      names = kColor;
      break;
    case RenderVarSource::kDepth:
      names = kDepth;
      break;
    case RenderVarSource::kNormal:
    case RenderVarSource::kPrimvar:
      break;
  }
  return names.first(std::min<std::size_t>(channels, names.size()));
}

}  // namespace umbra
