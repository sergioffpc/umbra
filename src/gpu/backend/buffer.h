// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// Memory on a GPU, owned.

#ifndef UMBRA_GPU_BACKEND_BUFFER_H_
#define UMBRA_GPU_BACKEND_BUFFER_H_

#include <driver_types.h>
#include <optix_types.h>

#include <cstddef>
#include <span>
#include <utility>

#include "support.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::gpu::backend {

// The pointer the CUDA runtime takes for `address`, on the GPU.
[[nodiscard]] void* PointerOf(CUdeviceptr address);

// Bytes in the memory of the current GPU, freed with it: the GPU it was
// allocated on must be current when it is freed. Empty when it holds none.
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

}  // namespace umbra::gpu::backend

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_GPU_BACKEND_BUFFER_H_
