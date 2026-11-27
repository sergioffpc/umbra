// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/ext/oidn/owners.h"

#include <OpenImageDenoise/oidn.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"

namespace umbra::ext::oidn {
namespace {

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;

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
std::expected<void, Error> CheckDevice(OIDNDevice UMBRA_NULLABLE device,
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

// Why OIDN could not create a device.
Error CreationError() {
  if (auto checked = CheckDevice(nullptr, "create its device");
      !checked.has_value()) {
    return std::move(checked.error());
  }
  return {ErrorCode::kUnavailable,
          "Open Image Denoise: cannot create its device"};
}

// What a device runs on.
enum class DeviceKind : std::uint8_t {
  kCpu,
  kCuda,
};

// Commits `device`, which OIDN created on `kind`, with its output off; on
// the CPU, with its threads left unpinned.
std::expected<void, Error> Commit(OIDNDevice device, DeviceKind kind) {
  oidnSetDeviceInt(device, "verbose", 0);
  if (kind == DeviceKind::kCpu) {
    oidnSetDeviceBool(device, "setAffinity", false);
  }
  oidnCommitDevice(device);
  return CheckDevice(device, "create its device");
}

// OIDN's format of `format`.
OIDNFormat FormatOf(ImageFormat format) {
  if (format == ImageFormat::kFloat3) {
    return OIDN_FORMAT_FLOAT3;
  }
  return OIDN_FORMAT_UNDEFINED;
}

// OIDN's quality of `quality`.
OIDNQuality QualityOf(Quality quality) {
  switch (quality) {
    case Quality::kDefault:
      return OIDN_QUALITY_DEFAULT;
    case Quality::kFast:
      return OIDN_QUALITY_FAST;
    case Quality::kBalanced:
      return OIDN_QUALITY_BALANCED;
    case Quality::kHigh:
      return OIDN_QUALITY_HIGH;
  }
  return OIDN_QUALITY_DEFAULT;
}

struct ReleaseDevice {
  void operator()(OIDNDevice device) const { oidnReleaseDevice(device); }
};
struct ReleaseBuffer {
  void operator()(OIDNBuffer buffer) const { oidnReleaseBuffer(buffer); }
};
struct ReleaseFilter {
  void operator()(OIDNFilter filter) const { oidnReleaseFilter(filter); }
};

// The OIDN handle `handle` owns; null when it is.
template <typename Handle>
auto Get(const std::unique_ptr<Handle>& handle)
    -> decltype(handle->owned.get()) {
  return handle == nullptr ? nullptr : handle->owned.get();
}

}  // namespace

struct Device::Handle {
  std::unique_ptr<OIDNDeviceImpl, ReleaseDevice> owned;
};

struct Buffer::Handle {
  std::unique_ptr<OIDNBufferImpl, ReleaseBuffer> owned;
};

struct Filter::Handle {
  std::unique_ptr<OIDNFilterImpl, ReleaseFilter> owned;
};

Device::Device(std::unique_ptr<Handle> handle) : handle_(std::move(handle)) {}
Device::Device(Device&& other) noexcept = default;
Device& Device::operator=(Device&& other) noexcept = default;
Device::~Device() = default;

Buffer::Buffer() = default;
Buffer::Buffer(std::unique_ptr<Handle> UMBRA_NULLABLE handle)
    : handle_(std::move(handle)) {}
Buffer::Buffer(Buffer&& other) noexcept = default;
Buffer& Buffer::operator=(Buffer&& other) noexcept = default;
Buffer::~Buffer() = default;

Filter::Filter() = default;
Filter::Filter(std::unique_ptr<Handle> UMBRA_NULLABLE handle)
    : handle_(std::move(handle)) {}
Filter::Filter(Filter&& other) noexcept = default;
Filter& Filter::operator=(Filter&& other) noexcept = default;
Filter::~Filter() = default;

std::expected<Device, Error> Device::CreateCpu() {
  OIDNDevice created = oidnNewDevice(OIDN_DEVICE_TYPE_CPU);
  if (created == nullptr) {
    return std::unexpected(CreationError());
  }
  Device device(
      std::make_unique<Device::Handle>(Device::Handle{.owned{created}}));
  return Commit(created, DeviceKind::kCpu).transform([&device] {
    return std::move(device);
  });
}

std::expected<Device, Error> Device::CreateCuda(int ordinal) {
  // The default stream.
  cudaStream_t stream = nullptr;
  OIDNDevice created = oidnNewCUDADevice(&ordinal, &stream, 1);
  if (created == nullptr) {
    return std::unexpected(CreationError());
  }
  Device device(
      std::make_unique<Device::Handle>(Device::Handle{.owned{created}}));
  return Commit(created, DeviceKind::kCuda).transform([&device] {
    return std::move(device);
  });
}

std::expected<void, Error> Device::Check(std::string_view what) const {
  return CheckDevice(Get(handle_), what);
}

int Device::threads() const {
  return oidnGetDeviceInt(Get(handle_), "numThreads");
}

Buffer Buffer::Create(const Device& device, std::size_t bytes) {
  OIDNBuffer created = oidnNewBuffer(Get(device.handle_), bytes);
  if (created == nullptr) {
    return {};
  }
  return Buffer(std::make_unique<Handle>(Handle{.owned{created}}));
}

void Buffer::Write(std::span<const float> values) {
  oidnWriteBuffer(Get(handle_), 0, values.size_bytes(), values.data());
}

void Buffer::Read(std::span<float> values) const {
  oidnReadBuffer(Get(handle_), 0, values.size_bytes(), values.data());
}

Filter Filter::Create(const Device& device, const char* type) {
  OIDNFilter created = oidnNewFilter(Get(device.handle_), type);
  if (created == nullptr) {
    return {};
  }
  return Filter(std::make_unique<Handle>(Handle{.owned{created}}));
}

void Filter::Enable(const char* name) {
  oidnSetFilterBool(Get(handle_), name, true);
}

void Filter::SetQuality(Quality quality) {
  oidnSetFilterInt(Get(handle_), "quality", QualityOf(quality));
}

void Filter::SetProgressMonitor(ProgressMonitor monitor,
                                void* UMBRA_NULLABLE user) {
  oidnSetFilterProgressMonitorFunction(Get(handle_), monitor, user);
}

void Filter::SetImage(const char* name, const Image& image) {
  oidnSetFilterImage(
      Get(handle_), name,
      image.buffer == nullptr ? nullptr : Get(image.buffer->handle_),
      FormatOf(image.format), image.width, image.height, 0, image.pixel_stride,
      0);
}

void Filter::Commit() { oidnCommitFilter(Get(handle_)); }

void Filter::Execute() { oidnExecuteFilter(Get(handle_)); }

}  // namespace umbra::ext::oidn
