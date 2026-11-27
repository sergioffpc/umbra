// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/ext/oidn/oidn.h"

#include <OpenImageDenoise/config.h>
#include <OpenImageDenoise/oidn.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <ranges>
#include <string_view>
#include <utility>

#include "umbra/core/error.h"
#include "umbra/core/log.h"
#include "umbra/core/number_format.h"
#include "umbra/core/progress.h"
#include "umbra/core/safety.h"

namespace umbra::ext::oidn {
namespace {

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;

struct ReleaseDevice {
  void operator()(OIDNDevice device) const { oidnReleaseDevice(device); }
};
struct ReleaseFilter {
  void operator()(OIDNFilter filter) const { oidnReleaseFilter(filter); }
};
struct ReleaseBuffer {
  void operator()(OIDNBuffer buffer) const { oidnReleaseBuffer(buffer); }
};

using DevicePtr = std::unique_ptr<OIDNDeviceImpl, ReleaseDevice>;
using FilterPtr = std::unique_ptr<OIDNFilterImpl, ReleaseFilter>;
using BufferPtr = std::unique_ptr<OIDNBufferImpl, ReleaseBuffer>;

// The bytes of an RGBA pixel, as the color and the output hold it, and of
// an RGB one, as the albedo and the normal hold it.
constexpr std::size_t kRgbaBytes = 4 * sizeof(float);
constexpr std::size_t kRgbBytes = 3 * sizeof(float);

// The error code of an OIDN error.
ErrorCode CodeOf(OIDNError error) {
  switch (error) {
    case OIDN_ERROR_OUT_OF_MEMORY:
    case OIDN_ERROR_UNSUPPORTED_HARDWARE:
      return ErrorCode::kUnavailable;
    default:
      break;
  }
  return ErrorCode::kInternal;
}

// The error OIDN reports for `device`, or for the process when `device` is
// null, since it was last asked, as the failure to do `what`.
std::expected<void, Error> Check(OIDNDevice UMBRA_NULLABLE device,
                                 std::string_view what) {
  const char* message = nullptr;
  const OIDNError error = oidnGetDeviceError(device, &message);
  if (error == OIDN_ERROR_NONE) {
    return {};
  }
  return std::unexpected(
      Error(CodeOf(error),
            std::format("Open Image Denoise: cannot {}: {}", what,
                        message == nullptr ? std::string_view("unknown error")
                                           : std::string_view(message))));
}

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

  // OIDN's progress monitor, on its threads: adds the growth of `fraction`,
  // from 0 to 1, times the share of the FilterProgress `user`, then tells
  // OIDN to continue, always. It neither allocates nor locks.
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

// The OIDN device on `device`, created but not committed: its output off,
// and on the CPU its threads left unpinned.
std::expected<DevicePtr, Error> NewDevice(const DenoiseDevice& device) {
  OIDNDevice created = nullptr;
  if (device.cuda_ordinal.has_value()) {
    const int ordinal = *device.cuda_ordinal;
    // The default stream.
    cudaStream_t stream = nullptr;
    created = oidnNewCUDADevice(&ordinal, &stream, 1);
  } else {
    created = oidnNewDevice(OIDN_DEVICE_TYPE_CPU);
  }
  if (created == nullptr) {
    if (auto checked = Check(nullptr, "create its device");
        !checked.has_value()) {
      return std::unexpected(std::move(checked.error()));
    }
    return std::unexpected(
        Error(ErrorCode::kUnavailable,
              "Open Image Denoise: cannot create its device"));
  }
  DevicePtr owned(created);
  oidnSetDeviceInt(created, "verbose", 0);
  if (!device.cuda_ordinal.has_value()) {
    oidnSetDeviceBool(created, "setAffinity", false);
  }
  return owned;
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

struct Denoiser::State {
  DevicePtr device;
  // The prefilters of the albedo and the normal, the color's filter, and
  // their progress.
  FilterPtr albedo_filter;
  FilterPtr normal_filter;
  FilterPtr color_filter;
  FilterProgress albedo_progress;
  FilterProgress normal_progress;
  FilterProgress color_progress;
  // The buffers, in the device's memory, for width × height pixels.
  BufferPtr color;
  BufferPtr albedo;
  BufferPtr normal;
  BufferPtr output;
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
  void BindImages(std::size_t columns, std::size_t rows) const;
  // Copies the color, the albedo and the normal of `images` to the device.
  std::expected<void, Error> Upload(const DenoiseImages& images) const;
  // Runs the three filters over `pixels` pixels, counting them into `phase`.
  std::expected<void, Error> RunFilters(std::size_t pixels,
                                        core::ProgressPhase& phase);
  // Runs `filter`, counting `share` pixels into `phase` through `progress`.
  std::expected<void, Error> Run(OIDNFilter filter, FilterProgress& progress,
                                 std::uint64_t share,
                                 core::ProgressPhase& phase) const;
  // Copies the denoised color from the device into the output of `images`.
  std::expected<void, Error> Download(const DenoiseImages& images) const;
};

std::expected<void, Error> Denoiser::State::CreateFilters() {
  OIDNDevice oidn_device = device.get();
  albedo_filter.reset(oidnNewFilter(oidn_device, "RT"));
  normal_filter.reset(oidnNewFilter(oidn_device, "RT"));
  color_filter.reset(oidnNewFilter(oidn_device, "RT"));
  if (auto checked = Check(oidn_device, "create its filters");
      !checked.has_value()) {
    return checked;
  }
  OIDNFilter color_pass = color_filter.get();
  oidnSetFilterBool(color_pass, "hdr", true);
  oidnSetFilterBool(color_pass, "cleanAux", true);
  oidnSetFilterInt(color_pass, "quality", OIDN_QUALITY_HIGH);
  oidnSetFilterProgressMonitorFunction(
      albedo_filter.get(), FilterProgress::ShouldContinue, &albedo_progress);
  oidnSetFilterProgressMonitorFunction(
      normal_filter.get(), FilterProgress::ShouldContinue, &normal_progress);
  oidnSetFilterProgressMonitorFunction(
      color_pass, FilterProgress::ShouldContinue, &color_progress);
  return Check(oidn_device, "set up its filters");
}

std::expected<void, Error> Denoiser::State::Resize(
    const DenoiseImages& images) {
  if (images.width == width && images.height == height && color != nullptr) {
    return {};
  }
  const auto columns = static_cast<std::size_t>(images.width);
  const auto rows = static_cast<std::size_t>(images.height);
  if (auto allocated = Allocate(columns * rows); !allocated.has_value()) {
    return allocated;
  }
  BindImages(columns, rows);
  if (auto checked = Check(device.get(), "set up its filters");
      !checked.has_value()) {
    color.reset();
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
  OIDNDevice oidn_device = device.get();
  color.reset();
  albedo.reset();
  normal.reset();
  output.reset();
  width = 0;
  height = 0;
  color.reset(oidnNewBuffer(oidn_device, pixels * kRgbaBytes));
  albedo.reset(oidnNewBuffer(oidn_device, pixels * kRgbBytes));
  normal.reset(oidnNewBuffer(oidn_device, pixels * kRgbBytes));
  output.reset(oidnNewBuffer(oidn_device, pixels * kRgbaBytes));
  if (auto checked = Check(oidn_device, "allocate its buffers");
      !checked.has_value()) {
    color.reset();
    return checked;
  }
  return {};
}

void Denoiser::State::BindImages(std::size_t columns, std::size_t rows) const {
  OIDNFilter albedo_prefilter = albedo_filter.get();
  oidnSetFilterImage(albedo_prefilter, "albedo", albedo.get(),
                     OIDN_FORMAT_FLOAT3, columns, rows, 0, 0, 0);
  oidnSetFilterImage(albedo_prefilter, "output", albedo.get(),
                     OIDN_FORMAT_FLOAT3, columns, rows, 0, 0, 0);
  OIDNFilter normal_prefilter = normal_filter.get();
  oidnSetFilterImage(normal_prefilter, "normal", normal.get(),
                     OIDN_FORMAT_FLOAT3, columns, rows, 0, 0, 0);
  oidnSetFilterImage(normal_prefilter, "output", normal.get(),
                     OIDN_FORMAT_FLOAT3, columns, rows, 0, 0, 0);
  // The color and the output are RGBA: the filter reads and writes RGB.
  OIDNFilter color_pass = color_filter.get();
  oidnSetFilterImage(color_pass, "color", color.get(), OIDN_FORMAT_FLOAT3,
                     columns, rows, 0, kRgbaBytes, 0);
  oidnSetFilterImage(color_pass, "albedo", albedo.get(), OIDN_FORMAT_FLOAT3,
                     columns, rows, 0, 0, 0);
  oidnSetFilterImage(color_pass, "normal", normal.get(), OIDN_FORMAT_FLOAT3,
                     columns, rows, 0, 0, 0);
  oidnSetFilterImage(color_pass, "output", output.get(), OIDN_FORMAT_FLOAT3,
                     columns, rows, 0, kRgbaBytes, 0);
  oidnCommitFilter(albedo_prefilter);
  oidnCommitFilter(normal_prefilter);
  oidnCommitFilter(color_pass);
}

std::expected<void, Error> Denoiser::State::Upload(
    const DenoiseImages& images) const {
  oidnWriteBuffer(color.get(), 0, images.color.size_bytes(),
                  images.color.data());
  oidnWriteBuffer(albedo.get(), 0, images.albedo.size_bytes(),
                  images.albedo.data());
  oidnWriteBuffer(normal.get(), 0, images.normal.size_bytes(),
                  images.normal.data());
  return Check(device.get(), "copy the images to its device");
}

std::expected<void, Error> Denoiser::State::RunFilters(
    std::size_t pixels, core::ProgressPhase& phase) {
  const std::uint64_t third = pixels / 3;
  if (auto run = Run(albedo_filter.get(), albedo_progress, third, phase);
      !run.has_value()) {
    return run;
  }
  if (auto run = Run(normal_filter.get(), normal_progress, third, phase);
      !run.has_value()) {
    return run;
  }
  return Run(color_filter.get(), color_progress, pixels - (2 * third), phase);
}

std::expected<void, Error> Denoiser::State::Run(
    OIDNFilter filter, FilterProgress& progress, std::uint64_t share,
    core::ProgressPhase& phase) const {
  progress.Begin(phase, share);
  oidnExecuteFilter(filter);
  if (auto checked = Check(device.get(), "denoise"); !checked.has_value()) {
    return checked;
  }
  progress.Complete();
  return {};
}

std::expected<void, Error> Denoiser::State::Download(
    const DenoiseImages& images) const {
  oidnReadBuffer(output.get(), 0, images.output.size_bytes(),
                 images.output.data());
  return Check(device.get(), "copy the denoised color from its device");
}

std::expected<void, Error> Initialize() {
  UMBRA_LOG_DEBUG(kOidn, "Open Image Denoise {}", OIDN_VERSION_STRING);
  return {};
}

Denoiser::Denoiser(std::unique_ptr<State> state) : state_(std::move(state)) {}

Denoiser::~Denoiser() = default;

std::expected<std::unique_ptr<Denoiser>, Error> Denoiser::Create(
    const DenoiseDevice& device) {
  auto created = NewDevice(device);
  if (!created.has_value()) {
    return std::unexpected(std::move(created.error()));
  }
  auto state = std::make_unique<State>();
  state->device = *std::move(created);
  OIDNDevice oidn_device = state->device.get();
  oidnCommitDevice(oidn_device);
  if (auto checked = Check(oidn_device, "create its device").and_then([&state] {
        return state->CreateFilters();
      });
      !checked.has_value()) {
    return std::unexpected(std::move(checked.error()));
  }
  if (device.cuda_ordinal.has_value()) {
    UMBRA_LOG_DEBUG(kOidn, "denoising on {}, CUDA device {}", device.name,
                    *device.cuda_ordinal);
  } else {
    UMBRA_LOG_DEBUG(kOidn, "denoising on the CPU, {} threads",
                    oidnGetDeviceInt(oidn_device, "numThreads"));
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

}  // namespace umbra::ext::oidn
