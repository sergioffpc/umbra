// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The CPUs of the machine, one entry per NUMA node with CPUs, as Linux
// describes them in sysfs: the model, the cores and threads, the memory
// attached to the node, and the caches, with the cores grouped by their cache
// sizes. A render device on the CPU is one NUMA node
// (docs/adr/0004-render-devices.md).

#ifndef UMBRA_CORE_CPU_TOPOLOGY_H_
#define UMBRA_CORE_CPU_TOPOLOGY_H_

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::core {

// What a cache holds.
enum class CacheType : std::uint8_t {
  kData,
  kInstruction,
  kUnified,
};

// One cache level of a group of cores.
struct CacheLevel {
  int level = 0;
  CacheType type = CacheType::kUnified;
  std::uint64_t size_bytes = 0;
  // How many cores share one instance of it: 1 for a per-core cache.
  int cores_per_instance = 1;
  // How many instances the group's cores use.
  int instances = 0;
};

// Cores of a node whose caches have the same sizes at every level, as the
// P- and E-cores of a hybrid CPU do not.
struct CoreGroup {
  int cores = 0;
  // From L1 up, data before instruction.
  std::vector<CacheLevel> caches;
};

// The CPUs of one NUMA node.
struct CpuNode {
  // The node's number in sysfs.
  int node = 0;
  // The model name ("AMD Ryzen Threadripper PRO 3975WX"), or "unknown CPU".
  std::string model;
  int cores = 0;
  int threads = 0;
  // The memory attached to the node, or 0 when unknown.
  std::uint64_t memory_bytes = 0;
  // The cache line size, or 0 when unknown.
  int cache_line_bytes = 0;
  // In order of their first core.
  std::vector<CoreGroup> core_groups;
};

// The machine's NUMA nodes with CPUs, in node order, read from the sysfs and
// procfs under `root` ("/" on a running system; a test passes a directory
// tree of its own). A machine without NUMA in sysfs is one node, 0. Fails
// with kUnavailable when no CPU is described.
[[nodiscard]] std::expected<std::vector<CpuNode>, Error> ReadCpuNodes(
    const std::filesystem::path& root = "/");

// The widest vector ISA the running CPU supports: "AVX-512", "AVX2", "AVX"
// or "SSE".
[[nodiscard]] std::string_view CpuVectorIsa();

// The NUMA node a device's sysfs numa_node file names, from its text: a
// non-negative decimal, optionally followed by a newline. Nothing for a
// device attached to no node, which the kernel writes as -1, and for text
// that is not such a number (empty, negative, or past int's range).
[[nodiscard]] std::optional<int> ParseNumaNode(std::string_view text);

}  // namespace umbra::core

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_CORE_CPU_TOPOLOGY_H_
