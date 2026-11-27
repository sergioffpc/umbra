// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/device/film.h"

#include <algorithm>
#include <cstdint>
#include <optional>

#include "umbra/device/device.h"
#include "umbra/device/image.h"

namespace umbra::device {

std::optional<FilmLayout> PlanFilm(const FrameParameters& frame) {
  if (frame.vars.size() > kMaxFilmPlanes) {
    return std::nullopt;
  }
  const PixelWindow& window = frame.image.window;
  const std::uint64_t pixels =
      static_cast<std::uint64_t>(std::max(window.width(), 0)) *
      static_cast<std::uint64_t>(std::max(window.height(), 0));
  FilmLayout layout;
  for (const FilmVar& var : frame.vars) {
    layout.storage.at(layout.count) = {
        .offset = layout.floats,
        .channels = var.channels,
        .source = var.source,
    };
    ++layout.count;
    layout.floats += pixels * var.channels;
  }
  return layout;
}

}  // namespace umbra::device
