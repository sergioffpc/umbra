// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The progress of umbra's work on stderr, one line per phase
// (docs/adr/0002-cli-diagnostics-on-stderr.md):
//
//   Loading stage: 100% (3/3 layers), 0.4 s, done.
//   Syncing scene:  42% (2.10M/5.00M prims) | 1.2 Mprims/s, ETA 2.4 s
//
// A phase is a ProgressPhase: it shows its name, the work done and, once it
// knows it, the total, in its unit. Work is added from any thread through an
// atomic counter, which neither allocates nor locks; a timer thread of the
// phase formats and writes the line. On a terminal the line is redrawn in
// place, at most every 100 ms; otherwise a line is written at every 10%. A
// phase ends with its "done." or "failed." line.
//
//   core::ProgressPhase phase("Syncing scene", "prims", prims.size());
//   tbb::parallel_for(..., [&](...) { ...; phase.Add(1); });
//   phase.Finish();

#ifndef UMBRA_CORE_PROGRESS_H_
#define UMBRA_CORE_PROGRESS_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::core {

// Whether progress is shown: -q hides it.
enum class ProgressDisplay : std::uint8_t {
  kShown,
  kHidden,
};

// Whether a phase's line shows its throughput and, with a total, the time
// left, while it runs on a terminal.
enum class ProgressRate : std::uint8_t {
  kHidden,
  kShown,
};

// Configures the process's progress: hidden, or shown in place when stderr
// is a terminal and at every 10% otherwise. Call it once, at startup, before
// a phase begins. Progress is hidden until it is called.
void ConfigureProgress(ProgressDisplay display);

// A phase of umbra's work, shown from its construction to its end. One
// phase runs at a time. A phase destroyed before Finish() fails, so that a
// phase left on an error ends with "failed." before the caller reports the
// error.
class ProgressPhase {
 public:
  // The total of a phase that does not know it yet.
  static constexpr std::uint64_t kUnknownTotal =
      std::numeric_limits<std::uint64_t>::max();

  // A phase whose total is not known yet: it shows its count only, until
  // SetTotal() gives one.
  ProgressPhase(std::string_view name, std::string_view unit,
                ProgressRate rate = ProgressRate::kHidden);
  ProgressPhase(std::string_view name, std::string_view unit,
                std::uint64_t total, ProgressRate rate = ProgressRate::kHidden);
  ProgressPhase(const ProgressPhase&) = delete;
  ProgressPhase& operator=(const ProgressPhase&) = delete;
  ProgressPhase(ProgressPhase&&) = delete;
  ProgressPhase& operator=(ProgressPhase&&) = delete;
  ~ProgressPhase();

  // Sets the total, once it is known or when it grows. Callable from any
  // thread.
  void SetTotal(std::uint64_t total) [[clang::nonblocking]] {
    total_.store(total, std::memory_order_relaxed);
  }

  // Adds `count` to the work done. Callable from any thread, on the hot
  // path: it neither allocates nor locks.
  void Add(std::uint64_t count) [[clang::nonblocking]] {
    done_.fetch_add(count, std::memory_order_relaxed);
  }

  // Ends the phase with its "done." line: all its work is done.
  void Finish();

  // Ends the phase with its "failed." line.
  void Fail();

 private:
  // Where the phase is shown.
  enum class Output : std::uint8_t {
    kHidden,
    kTerminal,
    kLines,
  };

  enum class State : std::uint8_t {
    kRunning,
    kEnded,
  };

  // The timer thread: shows the line until the phase ends.
  void Run(const std::stop_token& stop);
  // Shows the line once: draws it on a terminal, or writes it when the work
  // reaches the next 10% after `step` tenths, which it then raises.
  void Show(std::uint64_t& step) const;
  // Stops the timer thread and writes the last line.
  void End(std::string_view line);
  // The line of the running phase, with the throughput and the time left
  // when the phase shows them.
  [[nodiscard]] std::string RunningLine(ProgressRate rate) const;

  std::string name_;
  std::string unit_;
  ProgressRate rate_;
  Output output_;
  State state_ = State::kRunning;
  std::chrono::steady_clock::time_point start_;
  std::atomic<std::uint64_t> done_{0};
  std::atomic<std::uint64_t> total_;
  // Last, so that it starts once the rest is initialized.
  std::jthread timer_;
};

}  // namespace umbra::core

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_CORE_PROGRESS_H_
