// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/ext/oidn/owners.h"

#include <OpenImageDenoise/oidn.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string_view>
#include <utility>

#include "umbra/core/error.h"
#include "umbra/core/handle_owner.h"
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

// OIDN's handles, which the owners hold as void*.
OIDNDevice AsDevice(void* UMBRA_NULLABLE device) {
  return static_cast<OIDNDevice>(device);
}
OIDNBuffer AsBuffer(void* UMBRA_NULLABLE buffer) {
  return static_cast<OIDNBuffer>(buffer);
}
OIDNFilter AsFilter(void* UMBRA_NULLABLE filter) {
  return static_cast<OIDNFilter>(filter);
}

}  // namespace

void Device::Release(void* device) { oidnReleaseDevice(AsDevice(device)); }

void Buffer::Release(void* buffer) { oidnReleaseBuffer(AsBuffer(buffer)); }

void Filter::Release(void* filter) { oidnReleaseFilter(AsFilter(filter)); }

std::expected<Device, Error> Device::CreateCpu() {
  OIDNDevice created = oidnNewDevice(OIDN_DEVICE_TYPE_CPU);
  if (created == nullptr) {
    return std::unexpected(CreationError());
  }
  Device device(created);
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
  Device device(created);
  return Commit(created, DeviceKind::kCuda).transform([&device] {
    return std::move(device);
  });
}

std::expected<void, Error> Device::Check(std::string_view what) const {
  return CheckDevice(AsDevice(handle_.get()), what);
}

int Device::threads() const {
  return oidnGetDeviceInt(AsDevice(handle_.get()), "numThreads");
}

Buffer Buffer::Create(const Device& device, std::size_t bytes) {
  OIDNBuffer created = oidnNewBuffer(AsDevice(device.handle_.get()), bytes);
  if (created == nullptr) {
    return {};
  }
  return Buffer(created);
}

void Buffer::Write(std::span<const float> values) {
  oidnWriteBuffer(AsBuffer(handle_.get()), 0, values.size_bytes(),
                  values.data());
}

void Buffer::Read(std::span<float> values) const {
  oidnReadBuffer(AsBuffer(handle_.get()), 0, values.size_bytes(),
                 values.data());
}

Filter Filter::Create(const Device& device, const char* type) {
  OIDNFilter created = oidnNewFilter(AsDevice(device.handle_.get()), type);
  if (created == nullptr) {
    return {};
  }
  return Filter(created);
}

void Filter::Enable(const char* name) {
  oidnSetFilterBool(AsFilter(handle_.get()), name, true);
}

void Filter::SetQuality(Quality quality) {
  oidnSetFilterInt(AsFilter(handle_.get()), "quality", QualityOf(quality));
}

void Filter::SetProgressMonitor(ProgressMonitor monitor,
                                void* UMBRA_NULLABLE user) {
  oidnSetFilterProgressMonitorFunction(AsFilter(handle_.get()), monitor, user);
}

void Filter::SetImage(const char* name, const Image& image) {
  oidnSetFilterImage(
      AsFilter(handle_.get()), name,
      image.buffer == nullptr ? nullptr : AsBuffer(image.buffer->handle_.get()),
      FormatOf(image.format), image.width, image.height, 0, image.pixel_stride,
      0);
}

void Filter::Commit() { oidnCommitFilter(AsFilter(handle_.get())); }

void Filter::Execute() { oidnExecuteFilter(AsFilter(handle_.get())); }

}  // namespace umbra::ext::oidn
