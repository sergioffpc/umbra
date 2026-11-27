// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/ext/embree/embree.h"

#include <embree4/rtcore_device.h>

#include <expected>
#include <format>
#include <string_view>

#include "umbra/core/error.h"
#include "umbra/core/handle_owner.h"
#include "umbra/core/log.h"
#include "umbra/core/safety.h"
#include "umbra/ext/embree/errors.h"
#include "umbra/ext/embree/owners.h"

namespace umbra::ext::embree {
namespace {

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;
using ::umbra::core::LogComponent;
using ::umbra::core::LogLevel;

// The process's Embree device, released at exit.
core::HandleOwner<RTCDevice, rtcReleaseDevice>& OwnedDevice() {
  static core::HandleOwner<RTCDevice, rtcReleaseDevice> device;
  return device;
}

// The device's error function: every error Embree reports.
void LogError(void* /*user*/, RTCError code, const char* message) {
  if (code == RTC_ERROR_NONE) {
    return;
  }
  core::Log(LogLevel::kError, LogComponent::kEmbree,
            std::format("{}: {}", rtcGetErrorString(code),
                        message == nullptr ? std::string_view()
                                           : std::string_view(message)));
}

}  // namespace

std::expected<void, Error> Initialize() {
  auto& device = OwnedDevice();
  if (device.get() != nullptr) {
    return {};
  }
  RTCDevice created = rtcNewDevice(nullptr);
  if (created == nullptr) {
    return std::unexpected(
        Error(ErrorCode::kUnavailable,
              std::format("Embree: cannot create the device: {}",
                          rtcGetErrorString(rtcGetDeviceError(nullptr)))));
  }
  rtcSetDeviceErrorFunction(created, LogError, nullptr);
  device.reset(created);
  const auto version =
      rtcGetDeviceProperty(created, RTC_DEVICE_PROPERTY_VERSION);
  UMBRA_LOG_DEBUG(kEmbree, "Embree {}.{}.{}", version / 10000,
                  version / 100 % 100, version % 100);
  return {};
}

RTCDevice UMBRA_NULLABLE ProcessDevice() { return OwnedDevice().get(); }

Error BuildError(RTCError code, std::string_view what) {
  return {ErrorCode::kUnavailable, std::format("Embree cannot build {}: {}",
                                               what, rtcGetErrorString(code))};
}

Error BuildError(RTCDevice device, std::string_view what) {
  return BuildError(rtcGetDeviceError(device), what);
}

std::expected<void, Error> CheckBuild(RTCDevice device, std::string_view what) {
  const RTCError code = rtcGetDeviceError(device);
  if (code == RTC_ERROR_NONE) {
    return {};
  }
  return std::unexpected(BuildError(code, what));
}

std::string_view NativeIsa() {
  RTCDevice device = OwnedDevice().get();
  if (device == nullptr) {
    return "none";
  }
  if (rtcGetDeviceProperty(device,
                           RTC_DEVICE_PROPERTY_NATIVE_RAY16_SUPPORTED) != 0) {
    return "AVX-512";
  }
  return "AVX2";
}

}  // namespace umbra::ext::embree
