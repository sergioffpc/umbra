// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "device_threads.h"

#include <expected>
#include <format>
#include <ranges>
#include <span>
#include <thread>
#include <vector>

#include "umbra/core/error.h"
#include "umbra/core/floating_point.h"
#include "umbra/device/device.h"

namespace umbra::delegate::internal {

namespace {

// The result of `work` on each device of `devices`, in their order, each
// run on a host thread of its own whose floating point is configured
// first; once all are done.
std::vector<std::expected<void, core::Error>> RunOnThreads(
    std::span<device::Device* const> devices, const DeviceWork& work) {
  std::vector<std::expected<void, core::Error>> results(devices.size());
  std::vector<std::jthread> threads;
  threads.reserve(devices.size());
  for (const auto& [device, result] : std::views::zip(devices, results)) {
    threads.emplace_back([&work, device, &result] {
      core::ConfigureThreadFloatingPoint();
      result = work(*device);
    });
  }
  // Joins every thread.
  threads.clear();
  return results;
}

}  // namespace

std::expected<void, core::Error> OnEachDevice(
    std::span<device::Device* const> devices, const DeviceWork& work) {
  const std::vector<std::expected<void, core::Error>> results =
      RunOnThreads(devices, work);
  for (const auto& [device, result] : std::views::zip(devices, results)) {
    if (!result.has_value()) {
      return std::unexpected(core::Error(
          result.error().code(), std::format("device {}: {}", device->name(),
                                             result.error().message())));
    }
  }
  return {};
}

}  // namespace umbra::delegate::internal
