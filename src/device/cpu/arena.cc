// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "arena.h"

#include <oneapi/tbb/info.h>
#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/task_scheduler_observer.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "umbra/core/error.h"
#include "umbra/core/floating_point.h"
#include "umbra/core/log.h"
#include "umbra/core/memory_policy.h"

namespace umbra::device::cpu::internal {
namespace {

// The memory policy of the calling thread before it entered a CPU device's
// arena, while it is in the arena.
std::optional<core::MemoryPolicy>& SavedPolicy() {
  thread_local std::optional<core::MemoryPolicy> policy;
  return policy;
}

// The constraints of the arena of NUMA node `node`: bound to it when
// oneTBB knows of it.
tbb::task_arena::constraints ConstraintsOf(int node) {
  const std::vector<tbb::numa_node_id> nodes = tbb::info::numa_nodes();
  if (std::ranges::contains(nodes, node)) {
    return tbb::task_arena::constraints(node);
  }
  return {};
}

}  // namespace

ArenaObserver::ArenaObserver(tbb::task_arena& arena, int node,
                             std::string_view name)
    : tbb::task_scheduler_observer(arena), node_(node), name_(name) {
  observe(true);
}

ArenaObserver::~ArenaObserver() { observe(false); }

// NOLINTNEXTLINE(umbra-bool-parameter): oneTBB's interface.
void ArenaObserver::on_scheduler_entry(bool /*is_worker*/) {
  core::ConfigureThreadFloatingPoint();
  auto bound = core::BindThreadMemory(node_);
  if (bound.has_value()) {
    SavedPolicy() = *bound;
  } else if (!warned_.exchange(true, std::memory_order_relaxed)) {
    UMBRA_LOG_WARNING(kDevice,
                      "{}: memory not bound to NUMA node {} ({}); it lies "
                      "where the kernel places it",
                      name_, node_, bound.error().message());
  }
}

// NOLINTNEXTLINE(umbra-bool-parameter): oneTBB's interface.
void ArenaObserver::on_scheduler_exit(bool /*is_worker*/) {
  std::optional<core::MemoryPolicy>& saved = SavedPolicy();
  if (saved.has_value()) {
    core::RestoreThreadMemory(*saved);
    saved.reset();
  }
}

Arena::Arena(int node, std::string_view name)
    : constraints_(ConstraintsOf(node)),
      arena_(constraints_),
      observer_(arena_, node, name) {
  arena_.initialize();
  UMBRA_LOG_DEBUG(kDevice, "{}: arena of {} threads, {}", name,
                  arena_.max_concurrency(),
                  constraints_.numa_id == tbb::task_arena::automatic
                      ? std::string("on no NUMA node")
                      : std::format("on NUMA node {}", constraints_.numa_id));
}

int Arena::concurrency() const { return arena_.max_concurrency(); }

}  // namespace umbra::device::cpu::internal
