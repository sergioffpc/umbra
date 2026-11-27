// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/ext/cuda/owners.h"

#include <cuda.h>
#include <cuda_runtime_api.h>
#include <driver_types.h>

#include <bit>
#include <cstddef>
#include <exception>
#include <expected>
#include <format>
#include <span>
#include <string_view>

#include "log.h"
#include "umbra/core/handle_owner.h"
#include "umbra/core/log.h"
#include "umbra/core/log_hook.h"
#include "umbra/ext/cuda/conversions.h"
#include "umbra/ext/cuda/errors.h"

namespace umbra::ext::cuda {
namespace {

// Fails when `bytes` bytes at `offset` do not fit in `size`.
Result<void> CheckFits(std::size_t bytes, std::size_t offset,
                       std::size_t size) {
  if (offset > size || bytes > size - offset) {
    return std::unexpected(
        std::format("a copy of {} bytes at {} overruns a buffer of {}", bytes,
                    offset, size));
  }
  return {};
}

}  // namespace

void DeviceBuffer::Free(Memory memory) {
  if (cudaFree(PointerOf(memory.address)) != cudaSuccess) {
    try {
      Logging().Log(core::LogLevel::kWarning,
                    std::format("CUDA: cannot free {} bytes: {}", memory.size,
                                cudaGetErrorString(cudaGetLastError())));
    } catch (const std::exception& error) {
      Logging().Log(core::LogLevel::kWarning, error.what());
    }
  }
}

Result<DeviceBuffer> DeviceBuffer::Allocate(std::size_t bytes) {
  if (bytes == 0) {
    return DeviceBuffer();
  }
  void* pointer = nullptr;
  if (auto allocated = Check(cudaMalloc(&pointer, bytes),
                             std::format("cannot allocate {} bytes", bytes));
      !allocated.has_value()) {
    return std::unexpected(allocated.error());
  }
  return DeviceBuffer(
      Memory{.address = std::bit_cast<CUdeviceptr>(pointer), .size = bytes});
}

Result<DeviceBuffer> DeviceBuffer::Upload(std::span<const std::byte> bytes,
                                          cudaStream_t stream) {
  auto buffer = Allocate(bytes.size());
  if (!buffer.has_value()) {
    return buffer;
  }
  if (auto written = buffer->Write(bytes, 0, stream); !written.has_value()) {
    return std::unexpected(written.error());
  }
  return buffer;
}

// NOLINTNEXTLINE(readability-make-member-function-const): writes its memory.
Result<void> DeviceBuffer::Write(std::span<const std::byte> bytes,
                                 std::size_t offset, cudaStream_t stream) {
  if (bytes.empty()) {
    return {};
  }
  if (auto fits = CheckFits(bytes.size(), offset, size()); !fits.has_value()) {
    return fits;
  }
  return Check(cudaMemcpyAsync(PointerOf(address() + offset), bytes.data(),
                               bytes.size(), cudaMemcpyHostToDevice, stream),
               std::format("cannot upload {} bytes", bytes.size()));
}

// NOLINTNEXTLINE(readability-make-member-function-const): writes its memory.
Result<void> DeviceBuffer::CopyFrom(const DeviceBuffer& source,
                                    std::size_t offset, cudaStream_t stream) {
  if (source.size() == 0) {
    return {};
  }
  if (auto fits = CheckFits(source.size(), offset, size()); !fits.has_value()) {
    return fits;
  }
  return Check(cudaMemcpyAsync(PointerOf(address() + offset),
                               PointerOf(source.address()), source.size(),
                               cudaMemcpyDeviceToDevice, stream),
               std::format("cannot copy {} bytes", source.size()));
}

void Stream::DestroyLogged(cudaStream_t stream) {
  try {
    if (auto destroyed =
            Check(cudaStreamDestroy(stream), "cannot destroy a stream");
        !destroyed.has_value()) {
      Logging().Log(core::LogLevel::kWarning, destroyed.error());
    }
  } catch (const std::exception& error) {
    Logging().Log(core::LogLevel::kWarning, error.what());
  }
}

Result<Stream> Stream::Create(std::string_view what) {
  cudaStream_t stream = nullptr;
  return Check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), what)
      .transform([&] { return Stream(stream); });
}

Result<void> Stream::Destroy(std::string_view what) {
  if (stream_.get() == nullptr) {
    return {};
  }
  return Check(cudaStreamDestroy(stream_.release()), what);
}

}  // namespace umbra::ext::cuda
