// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// How a CPU device renders a render product: its film and sampler's cache,
// in its node's memory, and the tiles it takes in batches, renders and
// copies into the render buffers
// (docs/adr/0014-frames-render-in-tiles-from-one-counter.md,
// docs/adr/0015-tiles-reach-the-render-buffers-as-they-finish.md).

#ifndef UMBRA_DEVICE_CPU_RENDER_H_
#define UMBRA_DEVICE_CPU_RENDER_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/device/cpu/cpu_device.h"
#include "umbra/device/device.h"
#include "umbra/device/film.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device::cpu::internal {

// What a CPU device renders with, besides its BVHs: the sampler's cache,
// empty until the first render, and the film, the planes of the render
// vars of the product rendering, one after another, each laid out as its
// render buffer.
struct Film {
  std::vector<std::byte> sampler_cache;
  std::vector<float> planes;
};

// Readies `film` to hold a film of `layout`, on the device's arena: its
// sampler's cache, the first time, and planes large enough. Logs, as the
// device `name` at debug level, the NUMA node of what it allocates.
void PrepareFilm(Film& film, const device::FilmLayout& layout,
                 std::string_view name);

// What the render of a product reads.
struct RenderInputs {
  const CpuDevice* device;
  device::PixelSampling sampling;
  // How many tiles it takes at a time.
  std::uint64_t batch_size;
};

// What a device renders a product into: the film's layout and the film.
struct FilmTarget {
  const device::FilmLayout* layout;
  std::span<float> film;
};

// Renders the tiles it takes from `tiles`, a batch at a time, on the arena
// it runs in, into `target`: it copies each tile into the render buffers of
// `frame`'s vars as soon as it is rendered, then finishes it.
[[nodiscard]] std::expected<void, core::Error> RenderBatches(
    const RenderInputs& inputs, const device::FrameParameters& frame,
    device::TileQueue& tiles, const FilmTarget& target);

}  // namespace umbra::device::cpu::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_CPU_RENDER_H_
