// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The NUMA memory policy of a thread, where the kernel places the pages the
// thread touches first, and the node a page lies on. A CPU device keeps its
// memory on its node through it
// (docs/adr/0013-devices-render-from-their-own-memory.md).

#ifndef UMBRA_CORE_MEMORY_POLICY_H_
#define UMBRA_CORE_MEMORY_POLICY_H_

#include <array>
#include <cstdint>
#include <expected>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::core {

// A thread's memory policy, as BindThreadMemory saves it.
struct MemoryPolicy {
  int mode = 0;
  // The nodes it names, one bit each, as the kernel's mask of unsigned longs
  // lays them out on a 64-bit Linux.
  std::array<std::uint64_t, 16> nodes{};
};

// Binds the memory the calling thread touches first to NUMA node `node`:
// gives its policy before, which RestoreThreadMemory puts back. Fails with
// kUnavailable, naming the kernel's reason, when the kernel refuses: without
// CAP_SYS_NICE, for one, under Docker's default seccomp profile.
[[nodiscard]] std::expected<MemoryPolicy, Error> BindThreadMemory(int node);

// Gives the calling thread back `policy`, which BindThreadMemory saved.
void RestoreThreadMemory(const MemoryPolicy& policy);

// The NUMA node of the page at `address`, which has been touched. Fails
// with kUnavailable, naming the kernel's reason, when the kernel cannot
// tell.
[[nodiscard]] std::expected<int, Error> NodeOfPage(const void* address);

}  // namespace umbra::core

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_CORE_MEMORY_POLICY_H_
