// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/core/memory_policy.h"

#include <linux/mempolicy.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"

namespace umbra::core {
namespace {

// The kernel's node masks are of unsigned longs, 64 bits on x86-64.
constexpr std::size_t kWordBits = sizeof(std::uint64_t) * CHAR_BIT;
// The bits of a node mask.
constexpr std::size_t kMaskBits =
    std::tuple_size_v<decltype(MemoryPolicy::nodes)> * kWordBits;

// The error of a system call `call` that set errno.
Error KernelError(std::string_view call) {
  return {ErrorCode::kUnavailable,
          std::format("{}: {}", call, std::generic_category().message(errno))};
}

// get_mempolicy(2), without libnuma.
std::expected<void, Error> GetMempolicy(int* UMBRA_NULLABLE mode,
                                        std::uint64_t* UMBRA_NULLABLE nodes,
                                        const void* UMBRA_NULLABLE address,
                                        unsigned int flags) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): syscall(2) is variadic.
  if (syscall(SYS_get_mempolicy, mode, nodes, kMaskBits, address, flags) != 0) {
    return std::unexpected(KernelError("get_mempolicy"));
  }
  return {};
}

// set_mempolicy(2), without libnuma. The kernel reads one bit fewer than
// it is told.
std::expected<void, Error> SetMempolicy(
    int mode, const std::uint64_t* UMBRA_NULLABLE nodes) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): syscall(2) is variadic.
  if (syscall(SYS_set_mempolicy, mode, nodes, kMaskBits + 1) != 0) {
    return std::unexpected(KernelError("set_mempolicy"));
  }
  return {};
}

}  // namespace

std::expected<MemoryPolicy, Error> BindThreadMemory(int node) {
  if (node < 0 || std::cmp_greater_equal(node, kMaskBits)) {
    return std::unexpected(Error(ErrorCode::kInvalidArgument,
                                 std::format("no NUMA node {}", node)));
  }
  MemoryPolicy before;
  if (auto got = GetMempolicy(&before.mode, before.nodes.data(), nullptr, 0);
      !got.has_value()) {
    return std::unexpected(got.error());
  }
  MemoryPolicy bound{.mode = MPOL_BIND, .nodes = {}};
  const auto bit = static_cast<std::size_t>(node);
  bound.nodes.at(bit / kWordBits) = std::uint64_t{1} << (bit % kWordBits);
  return SetMempolicy(bound.mode, bound.nodes.data()).transform([&] {
    return before;
  });
}

void RestoreThreadMemory(const MemoryPolicy& policy) {
  // The policy was the thread's own, which the kernel took before.
  std::ignore = SetMempolicy(
      policy.mode, policy.mode == MPOL_DEFAULT ? nullptr : policy.nodes.data());
}

std::expected<int, Error> NodeOfPage(const void* address) {
  int node = 0;
  return GetMempolicy(&node, nullptr, address, MPOL_F_NODE | MPOL_F_ADDR)
      .transform([&] { return node; });
}

}  // namespace umbra::core
