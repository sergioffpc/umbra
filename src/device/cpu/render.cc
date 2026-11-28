// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "render.h"

#include <oneapi/tbb/parallel_for.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

#include "umbra/core/error.h"
#include "umbra/core/log.h"
#include "umbra/core/memory_policy.h"
#include "umbra/device/camera_rays.h"
#include "umbra/device/cpu/cpu_device.h"
#include "umbra/device/device.h"
#include "umbra/device/film.h"
#include "umbra/device/geometry.h"
#include "umbra/device/image.h"
#include "umbra/device/render_data.h"
#include "umbra/device/sampler.h"

namespace umbra::device::cpu::internal {
namespace {

// `vector` as Embree's rays hold it.
device::Float3 Float3Of(device::Vector3 vector) [[clang::nonblocking]] {
  return {.x = vector.x, .y = vector.y, .z = vector.z};
}

// The first hit of `ray` in what `device` traces.
device::FirstHit TraceFirstHit(const CpuDevice& device,
                               const device::CameraRay& ray)
    [[clang::nonblocking]] {
  const std::optional<Hit> hit = device.Intersect({
      .origin = Float3Of(ray.origin),
      .direction = Float3Of(ray.direction),
      .time = ray.time,
      .t_min = ray.t_min,
      .t_max = ray.t_max,
  });
  if (!hit.has_value()) {
    return {};
  }
  return {.distance = hit->distance, .normal = hit->normal};
}

// Renders `tile`'s pixels into the film's planes.
void RenderTile(const RenderInputs& inputs, const device::PixelWindow& window,
                const device::PixelWindow& tile,
                const device::FilmLayout& layout, std::span<float> film)
    [[clang::nonblocking]] {
  const auto trace =
      [&inputs](const device::CameraRay& ray)
          [[clang::nonblocking]] { return TraceFirstHit(*inputs.device, ray); };
  for (std::int32_t row = tile.y_min; row < tile.y_end; ++row) {
    for (std::int32_t column = tile.x_min; column < tile.x_end; ++column) {
      device::WritePixel(
          device::PlaceholderPixel(inputs.sampling, column, row, trace),
          device::PixelIndex(window, column, row), layout, film);
    }
  }
}

// Copies `source` into `destination`, which is as long.
void CopyFloats(std::span<const float> source, std::span<float> destination)
    [[clang::nonblocking]] {
  auto into = destination.begin();
  for (const float value : source.first(destination.size())) {
    *into = value;
    ++into;
  }
}

// Copies `tile`'s pixels from the film's planes into the render buffers of
// `frame`'s vars.
void CopyTile(const device::FrameParameters& frame,
              const device::PixelWindow& tile, const device::FilmLayout& layout,
              std::span<const float> film) [[clang::nonblocking]] {
  const auto tile_width = static_cast<std::size_t>(tile.width());
  for (const auto& [var, plane] :
       std::views::zip(frame.vars, layout.Planes())) {
    const std::size_t count = tile_width * plane.channels;
    for (std::int32_t row = tile.y_min; row < tile.y_end; ++row) {
      const std::size_t first =
          device::TileRowOffset(frame.image.window, tile, row, plane.channels);
      CopyFloats(film.subspan(plane.offset + first, count),
                 var.pixels.subspan(first, count));
    }
  }
}

// Logs, at debug level, the NUMA node of `what`, which `device` allocated
// and touched at `address`.
void LogNode(std::string_view device, std::string_view what,
             const void* address) {
  if (!core::IsLogEnabled(core::LogComponent::kDevice,
                          core::LogLevel::kDebug)) {
    return;
  }
  const auto node = core::NodeOfPage(address);
  UMBRA_LOG_DEBUG(kDevice, "{}: {} on NUMA node {}", device, what,
                  node.has_value()
                      ? std::format("{}", *node)
                      : std::format("unknown ({})", node.error().message()));
}

}  // namespace

void PrepareFilm(Film& film, const device::FilmLayout& layout,
                 std::string_view name) {
  if (film.sampler_cache.empty()) {
    film.sampler_cache.resize(device::kSamplerCacheSize);
    device::InitializeSamplerCache(film.sampler_cache);
    LogNode(name, "sampler cache", film.sampler_cache.data());
  }
  if (film.planes.size() < layout.floats) {
    film.planes = std::vector<float>(layout.floats);
    LogNode(name, "film", film.planes.data());
  }
}

std::expected<void, core::Error> RenderBatches(
    const RenderInputs& inputs, const device::FrameParameters& frame,
    device::TileQueue& tiles, const FilmTarget& target) {
  const auto render = [&](std::uint64_t tile) [[clang::nonblocking]] {
    const device::PixelWindow window = tiles.TileWindow(tile);
    RenderTile(inputs, frame.image.window, window, *target.layout, target.film);
    CopyTile(frame, window, *target.layout, target.film);
    tiles.Finish(tile);
  };
  return tiles.ForEachBatch(inputs.batch_size,
                            [&](const device::TileQueue::Batch& batch) {
                              tbb::parallel_for(batch.first, batch.end, render);
                              return std::expected<void, core::Error>();
                            });
}

}  // namespace umbra::device::cpu::internal
