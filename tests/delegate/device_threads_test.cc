// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "device_threads.h"

#include <gtest/gtest.h>

#include <array>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/device/change_set.h"
#include "umbra/device/device.h"

namespace umbra::delegate::internal {
namespace {

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;

// A device whose commit records the thread it runs on, and fails with
// `failure` when it has one.
class RecordingDevice final : public device::Device {
 public:
  RecordingDevice(std::string name, std::optional<Error> failure)
      : name_(std::move(name)), failure_(std::move(failure)) {}

  [[nodiscard]] std::string_view name() const UMBRA_LIFETIMEBOUND override {
    return name_;
  }

  [[nodiscard]] std::expected<void, Error> Commit(
      const device::ChangeSet& /*changes*/) override {
    ++commits_;
    thread_ = std::this_thread::get_id();
    if (failure_.has_value()) {
      return std::unexpected(*failure_);
    }
    return {};
  }

  [[nodiscard]] std::expected<void, Error> Render(
      const device::FrameParameters& /*frame*/,
      device::TileQueue& /*tiles*/) override {
    return {};
  }

  [[nodiscard]] int commits() const { return commits_; }
  [[nodiscard]] std::thread::id thread() const { return thread_; }

 private:
  std::string name_;
  std::optional<Error> failure_;
  int commits_ = 0;
  std::thread::id thread_;
};

// Commits an empty change set on each of `devices`.
std::expected<void, Error> CommitOnEach(
    std::span<device::Device* const> devices) {
  const device::ChangeSet changes{};
  return OnEachDevice(devices, [&changes](device::Device& device) {
    return device.Commit(changes);
  });
}

TEST(OnEachDeviceTest, RunsTheWorkOnceOnEveryDeviceOnAThreadOfItsOwn) {
  RecordingDevice first("cpu0", std::nullopt);
  RecordingDevice second("gpu0", std::nullopt);
  const std::array<device::Device*, 2> devices{&first, &second};

  const std::expected<void, Error> done = CommitOnEach(devices);

  ASSERT_TRUE(done.has_value());
  EXPECT_EQ(first.commits(), 1);
  EXPECT_EQ(second.commits(), 1);
  EXPECT_NE(first.thread(), std::this_thread::get_id());
  EXPECT_NE(second.thread(), std::this_thread::get_id());
  EXPECT_NE(first.thread(), second.thread());
}

TEST(OnEachDeviceTest, FailsWithTheFirstErrorInTheirOrderNamingItsDevice) {
  RecordingDevice first("cpu0", std::nullopt);
  RecordingDevice second("gpu0", Error(ErrorCode::kNotFound, "no module"));
  RecordingDevice third("gpu1", Error(ErrorCode::kInternal, "broken"));
  const std::array<device::Device*, 3> devices{&first, &second, &third};

  const std::expected<void, Error> done = CommitOnEach(devices);

  ASSERT_FALSE(done.has_value());
  EXPECT_EQ(done.error().code(), ErrorCode::kNotFound);
  EXPECT_EQ(done.error().message(), "device gpu0: no module");
  // Every device ran, the failing ones too.
  EXPECT_EQ(first.commits(), 1);
  EXPECT_EQ(third.commits(), 1);
}

}  // namespace
}  // namespace umbra::delegate::internal
