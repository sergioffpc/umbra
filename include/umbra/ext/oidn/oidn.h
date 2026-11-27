// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The bridge to Open Image Denoise, which denoises the color of a render
// product from its albedo and normal, on the whole image, once the devices
// have rendered it. It runs on OIDN's CUDA device on a GPU, or on its CPU
// device, whose arena takes the process's oneTBB threads
// (docs/adr/0001-onetbb-thread-pool.md). Its errors are returned, never
// printed: OIDN's own output is off.

#ifndef UMBRA_EXT_OIDN_OIDN_H_
#define UMBRA_EXT_OIDN_OIDN_H_

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "umbra/core/error.h"
#include "umbra/core/progress.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::oidn {

// Initializes Open Image Denoise for the process: logs its version at debug.
// Call it once at startup, after core::ConfigureLogging.
[[nodiscard]] std::expected<void, core::Error> Initialize();

// Where a denoiser runs: a GPU, or the CPU.
struct DenoiseDevice {
  // The CUDA ordinal of the GPU; none for the CPU.
  std::optional<int> cuda_ordinal;
  // umbra's name of the GPU ("gpu0"); empty for the CPU.
  std::string name;
};

// The images of a denoise, float32 over the same width × height pixels, row
// by row from the top left.
struct DenoiseImages {
  std::int32_t width = 0;
  std::int32_t height = 0;
  // The noisy color, RGBA.
  std::span<const float> color;
  // The albedo and the normal, RGB, noisy too.
  std::span<const float> albedo;
  std::span<const float> normal;
  // The denoised color, RGBA: the color's alpha is copied.
  std::span<float> output;
};

// A denoiser: an OIDN device and its filters, created once and reused for
// every denoise. Each denoise prefilters the albedo and the normal through
// an RT filter each, then filters the color through an RT filter for HDR
// images, at high quality, with those clean auxiliary images. Its buffers,
// in the device's memory, are reallocated only when the size of the images
// changes. One denoise runs at a time.
class Denoiser {
 public:
  // A denoiser on `device`. Fails with kUnavailable when OIDN cannot create
  // the device or its filters, with OIDN's cause.
  [[nodiscard]] static std::expected<std::unique_ptr<Denoiser>, core::Error>
  Create(const DenoiseDevice& device);

  Denoiser(const Denoiser&) = delete;
  Denoiser& operator=(const Denoiser&) = delete;
  Denoiser(Denoiser&&) = delete;
  Denoiser& operator=(Denoiser&&) = delete;
  ~Denoiser();

  // Denoises `images` into their output, counting the pixels of the three
  // filters into `progress`, a third of width × height each. Fails with
  // OIDN's cause: kUnavailable when memory runs out or the hardware is not
  // supported, kInternal otherwise.
  [[nodiscard]] std::expected<void, core::Error> Denoise(
      const DenoiseImages& images, core::ProgressPhase& progress);

  // Its state, with OIDN's types: the bridge's own.
  struct State;

  explicit Denoiser(std::unique_ptr<State> state);

 private:
  std::unique_ptr<State> state_;
};

}  // namespace umbra::ext::oidn

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_OIDN_OIDN_H_
