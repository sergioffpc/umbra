// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/ext/embree/embree.h"

#include <embree4/rtcore_device.h>

#include <expected>
#include <format>
#include <string_view>

#include "umbra/core/error.h"
#include "umbra/core/log.h"
#include "umbra/core/safety.h"

namespace umbra::ext::embree {
namespace {

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;
using ::umbra::core::LogComponent;
using ::umbra::core::LogLevel;

// The process's Embree device, released at exit.
class UMBRA_OWNER Device {
 public:
  Device() = default;
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;
  Device(Device&&) = delete;
  Device& operator=(Device&&) = delete;
  ~Device() {
    if (device_ != nullptr) {
      rtcReleaseDevice(device_);
    }
  }

  [[nodiscard]] RTCDevice get() const { return device_; }
  void reset(RTCDevice device) { device_ = device; }

 private:
  RTCDevice device_ = nullptr;
};

Device& ProcessDevice() {
  static Device device;
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
  Device& device = ProcessDevice();
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

std::string_view NativeIsa() {
  RTCDevice device = ProcessDevice().get();
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
