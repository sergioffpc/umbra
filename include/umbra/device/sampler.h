// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The sampler every device draws its samples from: OpenQMC's PmjBnSampler,
// progressive multi-jittered (0,2) sequences with blue noise between pixels,
// behind one alias. Its tables live in a cache that each device initializes
// once, in its own memory
// (docs/adr/0013-devices-render-from-their-own-memory.md); drawing a
// sample, single-source, neither allocates nor locks
// (docs/adr/0003-no-allocations-on-the-hot-path.md).

#ifndef UMBRA_DEVICE_SAMPLER_H_
#define UMBRA_DEVICE_SAMPLER_H_

#include <oqmc/pmjbn.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "umbra/core/safety.h"
#include "umbra/device/camera_rays.h"
#include "umbra/device/geometry.h"
#include "umbra/device/single_source.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device {

// The sampler of one sample of one pixel, from which the sample draws each
// of its dimensions in a domain of its own.
using Sampler = oqmc::PmjBnSampler;

// The size, in bytes, of a sampler's cache.
inline constexpr std::size_t kSamplerCacheSize = Sampler::cacheSize;

// Initializes `cache`, kSamplerCacheSize bytes aligned for std::uint32_t,
// in the memory of the device that draws from it.
inline void InitializeSamplerCache(std::span<std::byte> cache) {
  Sampler::initialiseCache(cache.data());
}

// The domains of a camera ray's samples.
enum class SampleDomain : std::uint8_t {
  kPixelFilter,
  kLens,
  kTime,
};

// The samples of a camera ray from `sampler`, that of one sample of a pixel.
UMBRA_HOST_DEVICE [[nodiscard]] inline CameraSamples DrawCameraSamples(
    const Sampler& sampler) UMBRA_NONBLOCKING {
  std::array<float, 2> filter{};
  std::array<float, 2> lens{};
  std::array<float, 1> time{};
  sampler.newDomain(static_cast<int>(SampleDomain::kPixelFilter))
      .drawSample<2>(filter.data());
  sampler.newDomain(static_cast<int>(SampleDomain::kLens))
      .drawSample<2>(lens.data());
  sampler.newDomain(static_cast<int>(SampleDomain::kTime))
      .drawSample<1>(time.data());
  return {
      .filter = SamplePixelFilter(filter.front(), filter.back()),
      .lens = {.x = lens.front(), .y = lens.back()},
      .time = time.front(),
  };
}

}  // namespace umbra::device

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_SAMPLER_H_
