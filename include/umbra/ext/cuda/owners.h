// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// Owners of what the CUDA runtime hands out: memory on a GPU, and streams.

#ifndef UMBRA_EXT_CUDA_OWNERS_H_
#define UMBRA_EXT_CUDA_OWNERS_H_

#include <cuda.h>
#include <driver_types.h>

#include <cstddef>
#include <span>
#include <string_view>
#include <utility>

#include "umbra/core/safety.h"
#include "umbra/ext/cuda/errors.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::cuda {

// Bytes in the memory of the current GPU, freed with it: the GPU it was
// allocated on must be current when it is freed. Empty when it holds none.
// A free that fails is logged as a warning.
class UMBRA_OWNER DeviceBuffer {
 public:
  DeviceBuffer() = default;
  DeviceBuffer(const DeviceBuffer&) = delete;
  DeviceBuffer& operator=(const DeviceBuffer&) = delete;
  DeviceBuffer(DeviceBuffer&& other) noexcept;
  DeviceBuffer& operator=(DeviceBuffer&& other) noexcept;
  ~DeviceBuffer();

  // `bytes` bytes, uninitialized; none for 0.
  [[nodiscard]] static Result<DeviceBuffer> Allocate(std::size_t bytes);

  // A copy of `bytes`, uploaded on `stream`.
  [[nodiscard]] static Result<DeviceBuffer> Upload(
      std::span<const std::byte> bytes, cudaStream_t stream);

  // A copy of `values`, uploaded on `stream`.
  template <typename T>
  [[nodiscard]] static Result<DeviceBuffer> UploadOf(std::span<const T> values,
                                                     cudaStream_t stream) {
    return Upload(std::as_bytes(values), stream);
  }

  // Uploads `bytes` on `stream` at `offset`, within the buffer.
  // NOLINTNEXTLINE(readability-make-member-function-const): writes its memory.
  [[nodiscard]] Result<void> Write(std::span<const std::byte> bytes,
                                   std::size_t offset, cudaStream_t stream);

  // Copies `source` on `stream` into the buffer at `offset`, within it.
  // NOLINTNEXTLINE(readability-make-member-function-const): writes its memory.
  [[nodiscard]] Result<void> CopyFrom(const DeviceBuffer& source,
                                      std::size_t offset, cudaStream_t stream);

  // Its address on the GPU; 0 when it is empty.
  [[nodiscard]] CUdeviceptr address() const { return address_; }
  [[nodiscard]] std::size_t size() const { return size_; }

 private:
  DeviceBuffer(CUdeviceptr address, std::size_t size)
      : address_(address), size_(size) {}

  CUdeviceptr address_ = 0;
  std::size_t size_ = 0;
};

// Uploads `values` on `stream` in place of what `buffer` holds.
template <typename T>
[[nodiscard]] Result<void> Replace(DeviceBuffer& buffer,
                                   std::span<const T> values,
                                   cudaStream_t stream) {
  return DeviceBuffer::UploadOf(values, stream)
      .transform([&](DeviceBuffer uploaded) { buffer = std::move(uploaded); });
}

// A stream of the current GPU that does not wait for the default stream,
// destroyed with it, or by Destroy. A destroy that fails in the destructor
// is logged as a warning.
class UMBRA_OWNER Stream {
 public:
  Stream() = default;
  Stream(const Stream&) = delete;
  Stream& operator=(const Stream&) = delete;
  Stream(Stream&& other) noexcept
      : stream_(std::exchange(other.stream_, nullptr)) {}
  Stream& operator=(Stream&& other) noexcept {
    std::swap(stream_, other.stream_);
    return *this;
  }
  ~Stream();

  // A stream of the current GPU.
  [[nodiscard]] static Result<Stream> Create(std::string_view what);

  // Destroys the stream now, if it holds one, and fails as the runtime
  // does.
  [[nodiscard]] Result<void> Destroy(std::string_view what);

  // The stream, which it must hold.
  [[nodiscard]] cudaStream_t get() const {
    return static_cast<cudaStream_t>(stream_);
  }

 private:
  explicit Stream(cudaStream_t UMBRA_NULLABLE stream) : stream_(stream) {}

  cudaStream_t UMBRA_NULLABLE stream_ = nullptr;
};

}  // namespace umbra::ext::cuda

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_CUDA_OWNERS_H_
