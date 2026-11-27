// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The film a device renders a product into, in its own memory
// (docs/adr/0013-devices-render-from-their-own-memory.md): one plane per
// render var, float32, each laid out as the var's render buffer, which the
// device copies each tile into once it is rendered
// (docs/adr/0015-tiles-reach-the-render-buffers-as-they-finish.md). The
// film's layout, its pixels and the placeholder integrator's pixel are
// single-source, and never allocate
// (docs/adr/0003-no-allocations-on-the-hot-path.md).

#ifndef UMBRA_DEVICE_FILM_H_
#define UMBRA_DEVICE_FILM_H_

#include <array>
#include <cstdint>
#include <optional>
#include <span>

#include "umbra/core/safety.h"
#include "umbra/device/camera_rays.h"
#include "umbra/device/geometry.h"
#include "umbra/device/image.h"
#include "umbra/device/light_shapes.h"
#include "umbra/device/sampler.h"
#include "umbra/device/single_source.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device {

// device.h: host code only.
struct FrameParameters;

// The most render vars a film holds a plane for.
inline constexpr std::uint32_t kMaxFilmPlanes = 16;

// Where a render var's plane lies in the film, and what it holds.
struct FilmPlane {
  // Its first float in the film.
  std::uint64_t offset = 0;
  std::uint32_t channels = 1;
  VarSource source = VarSource::kColor;
};

// The planes of a film, in the order of its product's vars, and the floats
// they take: plain data, which a GPU reads as the host writes it.
struct FilmLayout {
  // Room for kMaxFilmPlanes planes: the first `count` are the film's.
  std::array<FilmPlane, kMaxFilmPlanes> storage{};
  std::uint32_t count = 0;
  std::uint64_t floats = 0;

  UMBRA_HOST_DEVICE [[nodiscard]] std::span<const FilmPlane> Planes() const
      UMBRA_NONBLOCKING {
    return std::span(storage).first(count);
  }
};

// The film's layout for the vars of `frame`, over its data window; none
// when it has more than kMaxFilmPlanes vars.
[[nodiscard]] std::optional<FilmLayout> PlanFilm(const FrameParameters& frame);

// The index of the pixel at `column` and `row` in a plane over `window`:
// the pixels row by row from its top left.
UMBRA_HOST_DEVICE [[nodiscard]] inline std::uint64_t PixelIndex(
    const PixelWindow& window, std::int32_t column,
    std::int32_t row) UMBRA_NONBLOCKING {
  return (static_cast<std::uint64_t>(row - window.y_min) *
          static_cast<std::uint64_t>(window.width())) +
         static_cast<std::uint64_t>(column - window.x_min);
}

// The first float of `tile`'s pixels on `row`, in a plane of `channels`
// over `window`, and in its var's render buffer, which is laid out alike.
UMBRA_HOST_DEVICE [[nodiscard]] inline std::uint64_t TileRowOffset(
    const PixelWindow& window, const PixelWindow& tile, std::int32_t row,
    std::uint32_t channels) UMBRA_NONBLOCKING {
  return PixelIndex(window, tile.x_min, row) * channels;
}

// The values of a pixel the placeholder integrator renders, each the mean
// of its samples: the color 0.5·direction + 0.5, with alpha 1; the distance
// to the first hit, in meters, or kNoHit when a ray meets nothing; the
// geometric normal of the first hit, in world space, facing the ray, or 0.
struct PixelValue {
  std::array<float, 4> color{};
  float depth = 0.0F;
  std::array<float, 3> normal{};
};

// The channels a pixel's value gives a var of `source`: none for those the
// placeholder integrator leaves at 0.
UMBRA_HOST_DEVICE [[nodiscard]] inline std::span<const float> ChannelsOf(
    const PixelValue& value, VarSource source) UMBRA_NONBLOCKING {
  switch (source) {
    case VarSource::kColor:
      return value.color;
    case VarSource::kDepth:
      return {&value.depth, 1};
    case VarSource::kNormal:
      return value.normal;
    case VarSource::kAlbedo:
      break;
  }
  return {};
}

// Writes `value`, the pixel at `pixel` of the data window, into each plane
// of `layout` in `film`: the channels its var takes, and 0 in those it has
// beyond them.
UMBRA_HOST_DEVICE inline void WritePixel(
    const PixelValue& value, std::uint64_t pixel, const FilmLayout& layout,
    std::span<float> film) UMBRA_NONBLOCKING {
  for (const FilmPlane& plane : layout.Planes()) {
    const std::span<float> out =
        film.subspan(plane.offset + (pixel * plane.channels), plane.channels);
    const std::span<const float> given = ChannelsOf(value, plane.source);
    auto next = out.begin();
    for (const float channel :
         given.first(given.size() < out.size() ? given.size() : out.size())) {
      *next = channel;
      ++next;
    }
    for (; next != out.end(); ++next) {
      *next = 0.0F;
    }
  }
}

// The first hit of a camera ray, as a device's trace finds it: its
// distance, kNoHit when it meets nothing, and its geometric normal, in
// world space, of unit length, facing the ray.
struct FirstHit {
  float distance = kNoHit;
  Vector3 normal;
};

// What the pixels of a product are sampled with: its camera model, the
// device's sampler cache and the samples each pixel takes.
struct PixelSampling {
  const CameraModel* camera;
  const void* sampler_cache;
  std::uint32_t samples_per_pixel;
};

// The placeholder integrator's value of the pixel at `column` and `row`,
// whose camera rays `trace`, callable as FirstHit(const CameraRay&), finds
// the first hits of; at least one sample, whatever `sampling` asks.
template <typename Trace>
UMBRA_HOST_DEVICE [[nodiscard]] inline PixelValue PlaceholderPixel(
    const PixelSampling& sampling, std::int32_t column, std::int32_t row,
    const Trace& trace) UMBRA_NONBLOCKING {
  const std::uint32_t samples =
      sampling.samples_per_pixel > 0 ? sampling.samples_per_pixel : 1U;
  Vector3 color;
  Vector3 normal;
  float depth = 0.0F;
  for (std::uint32_t index = 0; index < samples; ++index) {
    const Sampler sampler(column, row, 0, static_cast<int>(index),
                          sampling.sampler_cache);
    const CameraRay ray =
        GenerateRay(*sampling.camera, column, row, DrawCameraSamples(sampler));
    color = color +
            ((0.5F * ray.direction) + Vector3{.x = 0.5F, .y = 0.5F, .z = 0.5F});
    const FirstHit hit = trace(ray);
    if (hit.distance != kNoHit) {
      depth += hit.distance;
      normal = normal + hit.normal;
    } else {
      depth = kNoHit;
    }
  }
  const float scale = 1.0F / static_cast<float>(samples);
  return {
      .color = {scale * color.x, scale * color.y, scale * color.z, 1.0F},
      .depth = scale * depth,
      .normal = {scale * normal.x, scale * normal.y, scale * normal.z},
  };
}

}  // namespace umbra::device

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_FILM_H_
