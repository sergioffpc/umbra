// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "denoiser.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <ranges>
#include <span>
#include <utility>

#include "umbra/core/error.h"
#include "umbra/core/log.h"
#include "umbra/core/number_format.h"
#include "umbra/core/progress.h"
#include "umbra/core/safety.h"
#include "umbra/delegate/session.h"
#include "umbra/ext/oidn/owners.h"

namespace umbra::delegate::internal {
namespace {

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;
using ::umbra::ext::oidn::Buffer;
using ::umbra::ext::oidn::Filter;
using ::umbra::ext::oidn::Image;

// The bytes of an RGBA pixel, as the color and the output hold it, and of
// an RGB one, as the albedo and the normal hold it.
constexpr std::size_t kRgbaBytes = 4 * sizeof(float);
constexpr std::size_t kRgbBytes = 3 * sizeof(float);

// The progress of one filter: the share of the phase's pixels it counts,
// added as OIDN reports the fraction of its work done.
class FilterProgress {
 public:
  // Starts counting `share` pixels into `phase`.
  void Begin(core::ProgressPhase& phase, std::uint64_t share) {
    phase_ = &phase;
    share_ = share;
    counted_.store(0, std::memory_order_relaxed);
  }

  // Counts what the filter has not counted yet, once it is done.
  void Complete() {
    const std::uint64_t counted =
        counted_.exchange(share_, std::memory_order_relaxed);
    if (phase_ != nullptr && counted < share_) {
      phase_->Add(share_ - counted);
    }
  }

  // The filter's progress monitor, on OIDN's threads: adds the growth of
  // `fraction`, from 0 to 1, times the share of the FilterProgress `user`,
  // then tells OIDN to continue, always. It neither allocates nor locks.
  static bool ShouldContinue(void* UMBRA_NULLABLE user, double fraction)
      [[clang::nonblocking]] {
    auto* const progress = static_cast<FilterProgress*>(user);
    if (progress != nullptr) {
      progress->Advance(fraction);
    }
    return true;
  }

 private:
  // Adds the growth of `fraction` times the share.
  void Advance(double fraction) [[clang::nonblocking]] {
    if (phase_ == nullptr) {
      return;
    }
    const auto target = static_cast<std::uint64_t>(
        std::clamp(fraction, 0.0, 1.0) * static_cast<double>(share_));
    std::uint64_t counted = counted_.load(std::memory_order_relaxed);
    while (counted < target) {
      if (counted_.compare_exchange_weak(counted, target,
                                         std::memory_order_relaxed)) {
        phase_->Add(target - counted);
        return;
      }
    }
  }

  // Null until Begin.
  core::ProgressPhase* phase_ = nullptr;
  std::uint64_t share_ = 0;
  std::atomic<std::uint64_t> counted_{0};
};

// The OIDN device on `device`, committed.
std::expected<ext::oidn::Device, Error> NewDevice(const DenoiseDevice& device) {
  if (device.cuda_ordinal.has_value()) {
    return ext::oidn::Device::CreateCuda(*device.cuda_ordinal);
  }
  return ext::oidn::Device::CreateCpu();
}

// The pixels of `images`: 0 when its spans do not hold them.
std::size_t PixelsOf(const DenoiseImages& images) {
  if (images.width <= 0 || images.height <= 0) {
    return 0;
  }
  const std::size_t pixels = static_cast<std::size_t>(images.width) *
                             static_cast<std::size_t>(images.height);
  const bool fits =
      images.color.size() == pixels * 4 && images.albedo.size() == pixels * 3 &&
      images.normal.size() == pixels * 3 && images.output.size() == pixels * 4;
  return fits ? pixels : 0;
}

// Copies the alpha of the color of `images` into their output.
void CopyAlpha(const DenoiseImages& images) {
  for (auto&& [denoised_alpha, noisy_alpha] : std::views::zip(
           images.output | std::views::drop(3) | std::views::stride(4),
           images.color | std::views::drop(3) | std::views::stride(4))) {
    denoised_alpha = noisy_alpha;
  }
}

}  // namespace

DenoiseDevice DenoiseDeviceOf(std::span<const Gpu> gpus) {
  DenoiseDevice denoise_device;
  for (const Gpu& gpu : gpus) {
    if (!denoise_device.cuda_ordinal.has_value() ||
        gpu.cuda_ordinal < *denoise_device.cuda_ordinal) {
      denoise_device = {.cuda_ordinal = gpu.cuda_ordinal, .name = gpu.name};
    }
  }
  return denoise_device;
}

struct Denoiser::State {
  explicit State(ext::oidn::Device created) : device(std::move(created)) {}

  ext::oidn::Device device;
  // The prefilters of the albedo and the normal, the color's filter, and
  // their progress.
  Filter albedo_filter;
  Filter normal_filter;
  Filter color_filter;
  FilterProgress albedo_progress;
  FilterProgress normal_progress;
  FilterProgress color_progress;
  // The buffers, in the device's memory, for width × height pixels.
  Buffer color;
  Buffer albedo;
  Buffer normal;
  Buffer output;
  std::int32_t width = 0;
  std::int32_t height = 0;

  // Creates the filters, with their parameters and progress monitors.
  std::expected<void, Error> CreateFilters();
  // Sizes the buffers for `images`, if they are not, and binds them to the
  // filters.
  std::expected<void, Error> Resize(const DenoiseImages& images);
  // Allocates the buffers for `pixels` pixels.
  std::expected<void, Error> Allocate(std::size_t pixels);
  // Binds the buffers to the filters, and commits them.
  void BindImages(std::size_t columns, std::size_t rows);
  // Copies the color, the albedo and the normal of `images` to the device.
  std::expected<void, Error> Upload(const DenoiseImages& images);
  // Runs the three filters over `pixels` pixels, counting them into `phase`.
  std::expected<void, Error> RunFilters(std::size_t pixels,
                                        core::ProgressPhase& phase);
  // Runs `filter`, counting `share` pixels into `phase` through `progress`.
  std::expected<void, Error> Run(Filter& filter, FilterProgress& progress,
                                 std::uint64_t share,
                                 core::ProgressPhase& phase) const;
  // Copies the denoised color from the device into the output of `images`.
  std::expected<void, Error> Download(const DenoiseImages& images) const;
};

std::expected<void, Error> Denoiser::State::CreateFilters() {
  albedo_filter = Filter::Create(device, "RT");
  normal_filter = Filter::Create(device, "RT");
  color_filter = Filter::Create(device, "RT");
  if (auto checked = device.Check("create its filters"); !checked.has_value()) {
    return checked;
  }
  color_filter.Enable("hdr");
  color_filter.Enable("cleanAux");
  color_filter.SetQuality(ext::oidn::Quality::kHigh);
  albedo_filter.SetProgressMonitor(FilterProgress::ShouldContinue,
                                   &albedo_progress);
  normal_filter.SetProgressMonitor(FilterProgress::ShouldContinue,
                                   &normal_progress);
  color_filter.SetProgressMonitor(FilterProgress::ShouldContinue,
                                  &color_progress);
  return device.Check("set up its filters");
}

std::expected<void, Error> Denoiser::State::Resize(
    const DenoiseImages& images) {
  if (images.width == width && images.height == height && !color.IsEmpty()) {
    return {};
  }
  const auto columns = static_cast<std::size_t>(images.width);
  const auto rows = static_cast<std::size_t>(images.height);
  if (auto allocated = Allocate(columns * rows); !allocated.has_value()) {
    return allocated;
  }
  BindImages(columns, rows);
  if (auto checked = device.Check("set up its filters"); !checked.has_value()) {
    color = Buffer();
    return checked;
  }
  width = images.width;
  height = images.height;
  UMBRA_LOG_DEBUG(
      kOidn, "buffers for {}x{} pixels, {}", width, height,
      core::FormatBytes(2 * columns * rows * (kRgbaBytes + kRgbBytes)));
  return {};
}

std::expected<void, Error> Denoiser::State::Allocate(std::size_t pixels) {
  color = Buffer();
  albedo = Buffer();
  normal = Buffer();
  output = Buffer();
  width = 0;
  height = 0;
  color = Buffer::Create(device, pixels * kRgbaBytes);
  albedo = Buffer::Create(device, pixels * kRgbBytes);
  normal = Buffer::Create(device, pixels * kRgbBytes);
  output = Buffer::Create(device, pixels * kRgbaBytes);
  if (auto checked = device.Check("allocate its buffers");
      !checked.has_value()) {
    color = Buffer();
    return checked;
  }
  return {};
}

void Denoiser::State::BindImages(std::size_t columns, std::size_t rows) {
  // The albedo, the normal and the color as RGB images: the color and the
  // output are RGBA, of which the filter reads and writes RGB.
  const auto rgb = [columns, rows](const Buffer& buffer) {
    return Image{
        .buffer = &buffer,
        .width = columns,
        .height = rows,
    };
  };
  const auto rgb_of_rgba = [columns, rows](const Buffer& buffer) {
    return Image{
        .buffer = &buffer,
        .width = columns,
        .height = rows,
        .pixel_stride = kRgbaBytes,
    };
  };
  albedo_filter.SetImage("albedo", rgb(albedo));
  albedo_filter.SetImage("output", rgb(albedo));
  normal_filter.SetImage("normal", rgb(normal));
  normal_filter.SetImage("output", rgb(normal));
  color_filter.SetImage("color", rgb_of_rgba(color));
  color_filter.SetImage("albedo", rgb(albedo));
  color_filter.SetImage("normal", rgb(normal));
  color_filter.SetImage("output", rgb_of_rgba(output));
  albedo_filter.Commit();
  normal_filter.Commit();
  color_filter.Commit();
}

std::expected<void, Error> Denoiser::State::Upload(
    const DenoiseImages& images) {
  color.Write(images.color);
  albedo.Write(images.albedo);
  normal.Write(images.normal);
  return device.Check("copy the images to its device");
}

std::expected<void, Error> Denoiser::State::RunFilters(
    std::size_t pixels, core::ProgressPhase& phase) {
  const std::uint64_t third = pixels / 3;
  if (auto run = Run(albedo_filter, albedo_progress, third, phase);
      !run.has_value()) {
    return run;
  }
  if (auto run = Run(normal_filter, normal_progress, third, phase);
      !run.has_value()) {
    return run;
  }
  return Run(color_filter, color_progress, pixels - (2 * third), phase);
}

std::expected<void, Error> Denoiser::State::Run(
    Filter& filter, FilterProgress& progress, std::uint64_t share,
    core::ProgressPhase& phase) const {
  progress.Begin(phase, share);
  filter.Execute();
  if (auto checked = device.Check("denoise"); !checked.has_value()) {
    return checked;
  }
  progress.Complete();
  return {};
}

std::expected<void, Error> Denoiser::State::Download(
    const DenoiseImages& images) const {
  output.Read(images.output);
  return device.Check("copy the denoised color from its device");
}

Denoiser::Denoiser(std::unique_ptr<State> state) : state_(std::move(state)) {}

Denoiser::~Denoiser() = default;

std::expected<std::unique_ptr<Denoiser>, Error> Denoiser::Create(
    const DenoiseDevice& device) {
  auto created = NewDevice(device);
  if (!created.has_value()) {
    return std::unexpected(std::move(created.error()));
  }
  auto state = std::make_unique<State>(*std::move(created));
  if (auto filters = state->CreateFilters(); !filters.has_value()) {
    return std::unexpected(std::move(filters.error()));
  }
  if (device.cuda_ordinal.has_value()) {
    UMBRA_LOG_DEBUG(kOidn, "denoising on {}, CUDA device {}", device.name,
                    *device.cuda_ordinal);
  } else {
    UMBRA_LOG_DEBUG(kOidn, "denoising on the CPU, {} threads",
                    state->device.threads());
  }
  return std::make_unique<Denoiser>(std::move(state));
}

std::expected<void, Error> Denoiser::Denoise(const DenoiseImages& images,
                                             core::ProgressPhase& progress) {
  State& state = *state_;
  const std::size_t pixels = PixelsOf(images);
  if (pixels == 0) {
    return std::unexpected(Error(
        ErrorCode::kInternal,
        std::format("Open Image Denoise: images that are not {}x{} pixels",
                    images.width, images.height)));
  }
  if (auto resized = state.Resize(images); !resized.has_value()) {
    return resized;
  }
  if (auto uploaded = state.Upload(images); !uploaded.has_value()) {
    return uploaded;
  }
  if (auto run = state.RunFilters(pixels, progress); !run.has_value()) {
    return run;
  }
  if (auto downloaded = state.Download(images); !downloaded.has_value()) {
    return downloaded;
  }
  // The filter wrote RGB only: the alpha is the color's.
  CopyAlpha(images);
  return {};
}

}  // namespace umbra::delegate::internal
