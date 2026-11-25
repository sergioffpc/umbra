// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/core/cpu_topology.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "umbra/core/error.h"

namespace umbra::core {
namespace {

namespace fs = std::filesystem;

constexpr std::uint64_t kKibibyte = 1024;

// A file's content without its trailing whitespace, or nothing when it cannot
// be read.
std::optional<std::string> ReadText(const fs::path& path) {
  const std::ifstream file(path);
  if (!file) {
    return std::nullopt;
  }
  std::ostringstream content;
  content << file.rdbuf();
  std::string text = std::move(content).str();
  while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) {
    text.pop_back();
  }
  return text;
}

// The leading decimal number of `text`, or nothing.
std::optional<std::uint64_t> ParseNumber(std::string_view text) {
  std::uint64_t value = 0;
  const char* const begin = std::to_address(text.begin());
  const auto [end, error] =
      std::from_chars(begin, std::to_address(text.end()), value);
  if (error != std::errc() || end == begin) {
    return std::nullopt;
  }
  return value;
}

std::optional<int> ReadInt(const fs::path& path) {
  return ReadText(path).and_then([](const std::string& text) {
    return ParseNumber(text).transform(
        [](std::uint64_t value) { return static_cast<int>(value); });
  });
}

// The CPUs of a sysfs CPU list ("0-3,32-35"), in order; malformed ranges are
// skipped.
std::vector<int> ParseCpuList(std::string_view list) {
  std::vector<int> cpus;
  while (!list.empty()) {
    const std::string_view range = list.substr(0, list.find(','));
    list.remove_prefix(std::min(list.size(), range.size() + 1));
    const std::size_t dash = range.find('-');
    const auto first = ParseNumber(range.substr(0, dash));
    const auto last = dash == std::string_view::npos
                          ? first
                          : ParseNumber(range.substr(dash + 1));
    if (!first.has_value() || !last.has_value()) {
      continue;
    }
    for (std::uint64_t cpu = *first; cpu <= *last; ++cpu) {
      cpus.push_back(static_cast<int>(cpu));
    }
  }
  return cpus;
}

// A sysfs cache size ("32K", "16384K", "1M"), in bytes.
std::uint64_t ParseCacheSize(std::string_view text) {
  const std::uint64_t value = ParseNumber(text).value_or(0);
  if (text.ends_with('K')) {
    return value * kKibibyte;
  }
  if (text.ends_with('M')) {
    return value * kKibibyte * kKibibyte;
  }
  return value;
}

CacheType ParseCacheType(std::string_view text) {
  if (text == "Data") {
    return CacheType::kData;
  }
  if (text == "Instruction") {
    return CacheType::kInstruction;
  }
  return CacheType::kUnified;
}

fs::path CpuDirectory(const fs::path& root, int cpu) {
  return root / "sys/devices/system/cpu" / ("cpu" + std::to_string(cpu));
}

// A core, as its package, its die in the package and its core number in the
// die: core_id restarts on each die of a multi-die package.
using CoreKey = std::tuple<int, int, int>;

// The core of each CPU of a node.
using CoreMap = std::map<int, CoreKey>;

CoreKey CoreOf(const fs::path& root, int cpu) {
  const fs::path topology = CpuDirectory(root, cpu) / "topology";
  return {ReadInt(topology / "physical_package_id").value_or(0),
          ReadInt(topology / "die_id").value_or(0),
          ReadInt(topology / "core_id").value_or(cpu)};
}

CoreMap CoreMapOf(const fs::path& root, const std::vector<int>& cpus) {
  CoreMap cores;
  for (const int cpu : cpus) {
    cores.emplace(cpu, CoreOf(root, cpu));
  }
  return cores;
}

// One cache of one CPU, as sysfs describes it.
struct CpuCache {
  int level = 0;
  CacheType type = CacheType::kUnified;
  std::uint64_t size_bytes = 0;
  std::string shared_cpus;
};

// The caches of a CPU, from L1 up, data before instruction.
std::vector<CpuCache> CachesOf(const fs::path& root, int cpu) {
  std::vector<CpuCache> caches;
  for (int index = 0;; ++index) {
    const fs::path cache =
        CpuDirectory(root, cpu) / "cache" / ("index" + std::to_string(index));
    const std::optional<int> level = ReadInt(cache / "level");
    if (!level.has_value()) {
      break;
    }
    caches.push_back({
        .level = *level,
        .type = ParseCacheType(ReadText(cache / "type").value_or("")),
        .size_bytes = ParseCacheSize(ReadText(cache / "size").value_or("")),
        .shared_cpus = ReadText(cache / "shared_cpu_list").value_or(""),
    });
  }
  std::ranges::stable_sort(caches, {}, [](const CpuCache& cache) {
    return std::pair(cache.level, cache.type);
  });
  return caches;
}

// The NUMA nodes with CPUs, and their CPUs: those of sysfs, or one node 0
// with every online CPU when the kernel describes no node.
std::map<int, std::vector<int>> NodeCpus(const fs::path& root) {
  std::map<int, std::vector<int>> nodes;
  std::error_code error;
  for (const fs::directory_entry& entry :
       fs::directory_iterator(root / "sys/devices/system/node", error)) {
    const std::string name = entry.path().filename().string();
    const auto node = std::string_view(name).starts_with("node")
                          ? ParseNumber(std::string_view(name).substr(4))
                          : std::nullopt;
    std::vector<int> cpus =
        ParseCpuList(ReadText(entry.path() / "cpulist").value_or(""));
    if (node.has_value() && !cpus.empty()) {
      nodes.emplace(static_cast<int>(*node), std::move(cpus));
    }
  }
  if (nodes.empty()) {
    std::vector<int> online = ParseCpuList(
        ReadText(root / "sys/devices/system/cpu/online").value_or(""));
    if (!online.empty()) {
      nodes.emplace(0, std::move(online));
    }
  }
  return nodes;
}

// The model name of each CPU in /proc/cpuinfo.
std::map<int, std::string> CpuModels(const fs::path& root) {
  std::map<int, std::string> models;
  std::istringstream lines(ReadText(root / "proc/cpuinfo").value_or(""));
  int processor = -1;
  for (std::string line; std::getline(lines, line);) {
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos || colon + 2 > line.size()) {
      continue;
    }
    const std::string_view key = std::string_view(line).substr(0, colon);
    const std::string_view value = std::string_view(line).substr(colon + 2);
    if (key.starts_with("processor")) {
      processor = static_cast<int>(ParseNumber(value).value_or(0));
    } else if (key.starts_with("model name")) {
      models.emplace(processor, std::string(value));
    }
  }
  return models;
}

// The kibibytes after `label` in a meminfo file, in bytes, or 0.
std::uint64_t MemTotal(const fs::path& meminfo) {
  const std::string text = ReadText(meminfo).value_or("");
  const std::size_t label = text.find("MemTotal:");
  if (label == std::string::npos) {
    return 0;
  }
  std::string_view value = std::string_view(text).substr(label + 9);
  value.remove_prefix(std::min(value.find_first_not_of(' '), value.size()));
  return ParseNumber(value).value_or(0) * kKibibyte;
}

std::uint64_t NodeMemory(const fs::path& root, int node) {
  const std::uint64_t memory =
      MemTotal(root / "sys/devices/system/node" /
               ("node" + std::to_string(node)) / "meminfo");
  return memory != 0 ? memory : MemTotal(root / "proc/meminfo");
}

// A core of a node: its key and the caches of its first thread.
struct NodeCore {
  CoreKey key;
  std::vector<CpuCache> caches;
};

// The cores of `cpus`, in order of their first thread.
std::vector<NodeCore> CoresOf(const fs::path& root,
                              const std::vector<int>& cpus,
                              const CoreMap& core_map) {
  std::vector<NodeCore> cores;
  std::set<CoreKey> seen;
  for (const int cpu : cpus) {
    const CoreKey key = core_map.at(cpu);
    if (seen.insert(key).second) {
      cores.push_back({.key = key, .caches = CachesOf(root, cpu)});
    }
  }
  return cores;
}

// The level, type and size of each cache: what makes two cores alike.
std::vector<std::pair<int, std::uint64_t>> SignatureOf(const NodeCore& core) {
  std::vector<std::pair<int, std::uint64_t>> signature;
  signature.reserve(core.caches.size());
  for (const CpuCache& cache : core.caches) {
    signature.emplace_back((cache.level * 4) + static_cast<int>(cache.type),
                           cache.size_bytes);
  }
  return signature;
}

// The cores of the CPU list `shared` among the node's: a cache shared with
// another node (sub-NUMA clustering) counts only this node's cores.
int CoresSharing(const CoreMap& core_map, std::string_view shared) {
  std::set<CoreKey> cores;
  for (const int cpu : ParseCpuList(shared)) {
    if (const auto core = core_map.find(cpu); core != core_map.end()) {
      cores.insert(core->second);
    }
  }
  return std::max(1, static_cast<int>(cores.size()));
}

// The caches of a group of alike cores: each level of the first core, with
// how many cores share an instance and how many instances the group uses.
std::vector<CacheLevel> GroupCaches(const CoreMap& core_map,
                                    const std::vector<NodeCore>& cores) {
  const std::vector<CpuCache>& first = cores.front().caches;
  std::vector<CacheLevel> levels;
  levels.reserve(first.size());
  for (std::size_t index = 0; index < first.size(); ++index) {
    std::set<std::string> instances;
    for (const NodeCore& core : cores) {
      instances.insert(core.caches.at(index).shared_cpus);
    }
    const CpuCache& cache = first.at(index);
    levels.push_back({
        .level = cache.level,
        .type = cache.type,
        .size_bytes = cache.size_bytes,
        .cores_per_instance = CoresSharing(core_map, cache.shared_cpus),
        .instances = static_cast<int>(instances.size()),
    });
  }
  return levels;
}

// The node's cores grouped by their cache sizes, in order of their first
// core.
std::vector<CoreGroup> GroupCores(const CoreMap& core_map,
                                  const std::vector<NodeCore>& cores) {
  std::vector<std::vector<std::pair<int, std::uint64_t>>> signatures;
  std::vector<std::vector<NodeCore>> groups;
  for (const NodeCore& core : cores) {
    const auto signature = SignatureOf(core);
    const auto found = std::ranges::find(signatures, signature);
    const auto index = static_cast<std::size_t>(
        std::ranges::distance(signatures.begin(), found));
    if (found == signatures.end()) {
      signatures.push_back(signature);
      groups.emplace_back();
    }
    groups.at(index).push_back(core);
  }
  std::vector<CoreGroup> result;
  result.reserve(groups.size());
  for (const std::vector<NodeCore>& group : groups) {
    result.push_back({
        .cores = static_cast<int>(group.size()),
        .caches = GroupCaches(core_map, group),
    });
  }
  return result;
}

CpuNode ReadNode(const fs::path& root, int node, const std::vector<int>& cpus,
                 const std::map<int, std::string>& models) {
  const CoreMap core_map = CoreMapOf(root, cpus);
  const std::vector<NodeCore> cores = CoresOf(root, cpus, core_map);
  const auto model = models.find(cpus.front());
  return {
      .node = node,
      .model = model == models.end() ? "unknown CPU" : model->second,
      .cores = static_cast<int>(cores.size()),
      .threads = static_cast<int>(cpus.size()),
      .memory_bytes = NodeMemory(root, node),
      .cache_line_bytes = ReadInt(CpuDirectory(root, cpus.front()) /
                                  "cache/index0/coherency_line_size")
                              .value_or(0),
      .core_groups = GroupCores(core_map, cores),
  };
}

}  // namespace

std::expected<std::vector<CpuNode>, Error> ReadCpuNodes(
    const std::filesystem::path& root) {
  const std::map<int, std::vector<int>> nodes = NodeCpus(root);
  if (nodes.empty()) {
    return std::unexpected(Error(
        ErrorCode::kUnavailable,
        "no CPU is described in " + (root / "sys/devices/system").string()));
  }
  const std::map<int, std::string> models = CpuModels(root);
  std::vector<CpuNode> result;
  result.reserve(nodes.size());
  for (const auto& [node, cpus] : nodes) {
    result.push_back(ReadNode(root, node, cpus, models));
  }
  return result;
}

std::string_view CpuVectorIsa() {
  __builtin_cpu_init();
  if (__builtin_cpu_supports("avx512f")) {
    return "AVX-512";
  }
  if (__builtin_cpu_supports("avx2")) {
    return "AVX2";
  }
  if (__builtin_cpu_supports("avx")) {
    return "AVX";
  }
  return "SSE";
}

}  // namespace umbra::core
