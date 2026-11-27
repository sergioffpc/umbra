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
#include <utility>

#include "log.h"
#include "umbra/core/log.h"
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

DeviceBuffer::DeviceBuffer(DeviceBuffer&& other) noexcept
    : address_(std::exchange(other.address_, 0)),
      size_(std::exchange(other.size_, 0)) {}

DeviceBuffer& DeviceBuffer::operator=(DeviceBuffer&& other) noexcept {
  if (this != &other) {
    const DeviceBuffer freed(std::move(*this));
    address_ = std::exchange(other.address_, 0);
    size_ = std::exchange(other.size_, 0);
  }
  return *this;
}

DeviceBuffer::~DeviceBuffer() {
  if (address_ != 0 && cudaFree(PointerOf(address_)) != cudaSuccess) {
    try {
      Log(core::LogLevel::kWarning,
          std::format("CUDA: cannot free {} bytes: {}", size_,
                      cudaGetErrorString(cudaGetLastError())));
    } catch (const std::exception& error) {
      Log(core::LogLevel::kWarning, error.what());
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
  return DeviceBuffer(std::bit_cast<CUdeviceptr>(pointer), bytes);
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
  if (auto fits = CheckFits(bytes.size(), offset, size_); !fits.has_value()) {
    return fits;
  }
  return Check(cudaMemcpyAsync(PointerOf(address_ + offset), bytes.data(),
                               bytes.size(), cudaMemcpyHostToDevice, stream),
               std::format("cannot upload {} bytes", bytes.size()));
}

// NOLINTNEXTLINE(readability-make-member-function-const): writes its memory.
Result<void> DeviceBuffer::CopyFrom(const DeviceBuffer& source,
                                    std::size_t offset, cudaStream_t stream) {
  if (source.size_ == 0) {
    return {};
  }
  if (auto fits = CheckFits(source.size_, offset, size_); !fits.has_value()) {
    return fits;
  }
  return Check(
      cudaMemcpyAsync(PointerOf(address_ + offset), PointerOf(source.address_),
                      source.size_, cudaMemcpyDeviceToDevice, stream),
      std::format("cannot copy {} bytes", source.size_));
}

Stream::~Stream() {
  try {
    if (auto destroyed = Destroy("cannot destroy a stream");
        !destroyed.has_value()) {
      Log(core::LogLevel::kWarning, destroyed.error());
    }
  } catch (const std::exception& error) {
    Log(core::LogLevel::kWarning, error.what());
  }
}

Result<Stream> Stream::Create(std::string_view what) {
  cudaStream_t stream = nullptr;
  return Check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking), what)
      .transform([&] { return Stream(stream); });
}

Result<void> Stream::Destroy(std::string_view what) {
  if (stream_ == nullptr) {
    return {};
  }
  return Check(cudaStreamDestroy(std::exchange(stream_, nullptr)), what);
}

}  // namespace umbra::ext::cuda
