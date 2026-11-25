// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/core/progress.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <format>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

#include "stderr_output.h"
#include "umbra/core/floating_point.h"
#include "umbra/core/number_format.h"

namespace umbra::core {
namespace {

// How often the line is drawn on a terminal, and how often the work is
// checked for the next 10% otherwise.
constexpr std::chrono::milliseconds kInterval{100};
constexpr std::uint64_t kPercent = 100;
constexpr std::uint64_t kStepsPerPhase = 10;

// The percent of `total` that `done` is, rounded down, so that 100% means
// all of it.
std::uint64_t Percent(std::uint64_t done, std::uint64_t total) {
  if (done >= total) {
    return kPercent;
  }
  const auto percent = static_cast<std::uint64_t>(
      static_cast<double>(done) * static_cast<double>(kPercent) /
      static_cast<double>(total));
  return std::min(percent, kPercent - 1);
}

}  // namespace

void ConfigureProgress(ProgressDisplay display) {
  using Output = ProgressPhase::Output;
  Output output = Output::kHidden;
  if (display == ProgressDisplay::kShown) {
    output = isatty(STDERR_FILENO) == 1 ? Output::kTerminal : Output::kLines;
  }
  ProgressPhase::ConfiguredOutput().store(output, std::memory_order_relaxed);
}

std::atomic<ProgressPhase::Output>& ProgressPhase::ConfiguredOutput() {
  static std::atomic<Output> output{Output::kHidden};
  return output;
}

ProgressPhase::ProgressPhase(std::string_view name, std::string_view unit,
                             ProgressRate rate)
    : ProgressPhase(name, unit, kUnknownTotal, rate) {}

ProgressPhase::ProgressPhase(std::string_view name, std::string_view unit,
                             std::uint64_t total, ProgressRate rate)
    : name_(name),
      unit_(unit),
      rate_(rate),
      output_(ConfiguredOutput().load(std::memory_order_relaxed)),
      start_(std::chrono::steady_clock::now()),
      total_(total) {
  if (output_ == Output::kHidden) {
    return;
  }
  timer_ = std::jthread([this](const std::stop_token& stop) { Run(stop); });
}

ProgressPhase::~ProgressPhase() {
  if (state_ == State::kRunning) {
    Fail();
  }
}

std::string ProgressPhase::Status(std::uint64_t done,
                                  std::uint64_t total) const {
  if (total == kUnknownTotal) {
    return std::format("{}: {} {}", name_, FormatCount(done), unit_);
  }
  const std::uint64_t shown = std::min(done, total);
  return std::format("{}: {:3}% ({}/{} {})", name_, Percent(shown, total),
                     FormatCount(shown), FormatCount(total), unit_);
}

std::string ProgressPhase::RunningLine(ProgressRate rate) const {
  const std::uint64_t done = done_.load(std::memory_order_relaxed);
  const std::uint64_t total = total_.load(std::memory_order_relaxed);
  std::string line = Status(done, total);
  const std::chrono::duration<double> elapsed =
      std::chrono::steady_clock::now() - start_;
  if (rate == ProgressRate::kHidden || done == 0 || elapsed.count() <= 0.0) {
    return line;
  }
  const double per_second = static_cast<double>(done) / elapsed.count();
  line += " | " + FormatRate(per_second, unit_);
  if (total != kUnknownTotal && done < total) {
    const double left = static_cast<double>(total - done) / per_second;
    line += ", ETA " + FormatDuration(std::chrono::duration<double>(left));
  }
  return line;
}

void ProgressPhase::Run(const std::stop_token& stop) {
  ConfigureThreadFloatingPoint();
  std::mutex mutex;
  std::condition_variable_any wake;
  std::unique_lock lock(mutex);
  while (!stop.stop_requested()) {
    Show();
    wake.wait_for(lock, stop, kInterval, [] { return false; });
  }
}

void ProgressPhase::Show() {
  if (output_ == Output::kTerminal) {
    internal::DrawProgressLine(RunningLine(rate_));
    return;
  }
  const std::uint64_t total = total_.load(std::memory_order_relaxed);
  if (total == kUnknownTotal) {
    return;
  }
  const std::uint64_t done = done_.load(std::memory_order_relaxed);
  const std::uint64_t tenths = Percent(done, total) / kStepsPerPhase;
  // The 100% line is the "done." line.
  if (tenths > written_tenths_ && tenths < kStepsPerPhase) {
    written_tenths_ = tenths;
    internal::WriteAboveProgressLine(RunningLine(ProgressRate::kHidden) + "\n");
  }
}

void ProgressPhase::End(std::string_view line) {
  state_ = State::kEnded;
  if (output_ == Output::kHidden) {
    return;
  }
  timer_.request_stop();
  timer_.join();
  if (output_ == Output::kTerminal) {
    internal::EndProgressLine(line);
  } else {
    internal::WriteAboveProgressLine(std::string(line) + "\n");
  }
}

void ProgressPhase::Finish() {
  if (state_ == State::kEnded) {
    return;
  }
  const std::uint64_t done = done_.load(std::memory_order_relaxed);
  const std::uint64_t total = total_.load(std::memory_order_relaxed);
  const std::uint64_t all = total == kUnknownTotal ? done : total;
  const std::chrono::duration<double> elapsed =
      std::chrono::steady_clock::now() - start_;
  End(std::format("{}, {}, done.", Status(all, all), FormatDuration(elapsed)));
}

void ProgressPhase::Fail() {
  if (state_ == State::kEnded) {
    return;
  }
  End(RunningLine(ProgressRate::kHidden) + ", failed.");
}

}  // namespace umbra::core
