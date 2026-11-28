// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The oneTBB arena of a CPU device, bound to its NUMA node, so that every
// page its threads first touch lies on the node
// (docs/adr/0001-onetbb-thread-pool.md,
// docs/adr/0013-devices-render-from-their-own-memory.md).

#ifndef UMBRA_DEVICE_CPU_ARENA_H_
#define UMBRA_DEVICE_CPU_ARENA_H_

#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/task_scheduler_observer.h>

#include <atomic>
#include <string_view>

#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device::cpu::internal {

// Configures every thread that enters a CPU device's arena, its workers and
// the external thread alike: its floating-point state, and its memory
// policy, bound to the device's NUMA node, so that every page first touched
// in the arena lies on the node. A thread leaving the arena gets its own
// policy back. A thread is in one device's arena at a time.
class ArenaObserver final : public tbb::task_scheduler_observer {
 public:
  // Observes `arena`, of the device `name` of NUMA node `node`.
  ArenaObserver(tbb::task_arena& arena, int node, std::string_view name);
  ArenaObserver(const ArenaObserver&) = delete;
  ArenaObserver& operator=(const ArenaObserver&) = delete;
  ArenaObserver(ArenaObserver&&) = delete;
  ArenaObserver& operator=(ArenaObserver&&) = delete;
  ~ArenaObserver() override;

  // NOLINTNEXTLINE(umbra-bool-parameter): oneTBB's interface.
  void on_scheduler_entry(bool is_worker) override;

  // NOLINTNEXTLINE(umbra-bool-parameter): oneTBB's interface.
  void on_scheduler_exit(bool is_worker) override;

 private:
  int node_;
  std::string_view name_;
  std::atomic<bool> warned_{false};
};

// The arena of a CPU device: bound to its NUMA node when oneTBB knows of
// it, and over every core otherwise, as on a machine without NUMA.
class Arena {
 public:
  // The arena of NUMA node `node`, of the device `name`, which outlives it,
  // initialized. Logs, as the device component at debug level, its threads
  // and its node.
  Arena(int node, std::string_view name);
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  Arena(Arena&&) = delete;
  Arena& operator=(Arena&&) = delete;
  ~Arena() = default;

  // Runs `work` in the arena, on the calling thread and the arena's
  // workers, and waits for it.
  template <typename Work>
  void Execute(const Work& work) {
    arena_.execute(work);
  }

  // The threads it runs work on at once.
  [[nodiscard]] int concurrency() const;

 private:
  tbb::task_arena::constraints constraints_;
  tbb::task_arena arena_;
  ArenaObserver observer_;
};

}  // namespace umbra::device::cpu::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_CPU_ARENA_H_
