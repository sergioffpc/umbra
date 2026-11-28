// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The calls into the CUDA runtime that the GPU module makes, each failing
// with the runtime's message: what describes the GPUs, makes one current,
// waits for a stream and copies between the host and a GPU. A call that
// succeeds formats no message.

#ifndef UMBRA_EXT_CUDA_RUNTIME_H_
#define UMBRA_EXT_CUDA_RUNTIME_H_

#include <driver_types.h>

#include <cstddef>
#include <format>
#include <span>

#include "umbra/core/lazy_format.h"
#include "umbra/core/safety.h"
#include "umbra/ext/cuda/errors.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::cuda {

// Each call fails with the runtime's message, "CUDA: <what>: <message>
// (<name>)", and takes `what` either made (core::LazyFormat) or as a format
// string and its arguments, which it formats only when it fails.

// How many GPUs the runtime sees.
[[nodiscard]] Result<int> DeviceCount(const core::LazyFormat& what);

template <typename... Args>
[[nodiscard]] Result<int> DeviceCount(std::format_string<Args...> what,
                                      const Args&... args) {
  return DeviceCount(
      core::LazyFormat(what.get(), std::make_format_args(args...)));
}

// The properties of the GPU of ordinal `ordinal`. A failure leaves no error
// behind for the calls after it.
[[nodiscard]] Result<cudaDeviceProp> DeviceProperties(
    int ordinal, const core::LazyFormat& what);

template <typename... Args>
[[nodiscard]] Result<cudaDeviceProp> DeviceProperties(
    int ordinal, std::format_string<Args...> what, const Args&... args) {
  return DeviceProperties(
      ordinal, core::LazyFormat(what.get(), std::make_format_args(args...)));
}

// The major version of the compute capability of the GPU of ordinal
// `ordinal`.
[[nodiscard]] Result<int> ComputeCapabilityMajor(int ordinal,
                                                 const core::LazyFormat& what);

template <typename... Args>
[[nodiscard]] Result<int> ComputeCapabilityMajor(
    int ordinal, std::format_string<Args...> what, const Args&... args) {
  return ComputeCapabilityMajor(
      ordinal, core::LazyFormat(what.get(), std::make_format_args(args...)));
}

// Writes the PCI bus id of the GPU of ordinal `ordinal` into `bus_id`,
// NUL-terminated.
[[nodiscard]] Result<void> PciBusId(int ordinal, std::span<char> bus_id,
                                    const core::LazyFormat& what);

template <typename... Args>
[[nodiscard]] Result<void> PciBusId(int ordinal, std::span<char> bus_id,
                                    std::format_string<Args...> what,
                                    const Args&... args) {
  return PciBusId(ordinal, bus_id,
                  core::LazyFormat(what.get(), std::make_format_args(args...)));
}

// The version of the CUDA driver, as 1000 × major + 10 × minor.
[[nodiscard]] Result<int> DriverVersion(const core::LazyFormat& what);

template <typename... Args>
[[nodiscard]] Result<int> DriverVersion(std::format_string<Args...> what,
                                        const Args&... args) {
  return DriverVersion(
      core::LazyFormat(what.get(), std::make_format_args(args...)));
}

// Makes the GPU of ordinal `ordinal` current on the calling thread.
[[nodiscard]] Result<void> SetDevice(int ordinal, const core::LazyFormat& what);

template <typename... Args>
[[nodiscard]] Result<void> SetDevice(int ordinal,
                                     std::format_string<Args...> what,
                                     const Args&... args) {
  return SetDevice(
      ordinal, core::LazyFormat(what.get(), std::make_format_args(args...)));
}

// Waits for the work queued on `stream`.
[[nodiscard]] Result<void> Synchronize(cudaStream_t stream,
                                       const core::LazyFormat& what);

template <typename... Args>
[[nodiscard]] Result<void> Synchronize(cudaStream_t stream,
                                       std::format_string<Args...> what,
                                       const Args&... args) {
  return Synchronize(
      stream, core::LazyFormat(what.get(), std::make_format_args(args...)));
}

// A copy of `rows` rows of `row_bytes` bytes, from `from`, whose rows lie
// `from_pitch` bytes apart, into `to`, whose rows lie `to_pitch` bytes
// apart, as `kind` says which memory each is in.
struct Copy2D {
  void* UMBRA_NULLABLE to = nullptr;
  std::size_t to_pitch = 0;
  const void* UMBRA_NULLABLE from = nullptr;
  std::size_t from_pitch = 0;
  std::size_t row_bytes = 0;
  std::size_t rows = 0;
  cudaMemcpyKind kind = cudaMemcpyDefault;
};

// Queues `copy` on `stream`.
[[nodiscard]] Result<void> Copy2DAsync(const Copy2D& copy, cudaStream_t stream,
                                       const core::LazyFormat& what);

template <typename... Args>
[[nodiscard]] Result<void> Copy2DAsync(const Copy2D& copy, cudaStream_t stream,
                                       std::format_string<Args...> what,
                                       const Args&... args) {
  return Copy2DAsync(
      copy, stream,
      core::LazyFormat(what.get(), std::make_format_args(args...)));
}

}  // namespace umbra::ext::cuda

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_CUDA_RUNTIME_H_
