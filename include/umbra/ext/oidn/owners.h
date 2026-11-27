// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// Owners of Open Image Denoise's handles: its devices, the buffers in their
// memory and the filters that run on them, with the calls the render
// delegate's denoise makes through them. OIDN reports the errors of a
// device and of what it made through the device: Device::Check returns
// them.

#ifndef UMBRA_EXT_OIDN_OWNERS_H_
#define UMBRA_EXT_OIDN_OWNERS_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::oidn {

// An OIDN device, released when it is destroyed. Its output is off: its
// errors are returned by Check, never printed.
class UMBRA_OWNER Device {
 public:
  // OIDN's CPU device, committed, whose threads are left unpinned: its arena
  // takes the process's oneTBB threads (docs/adr/0001-onetbb-thread-pool.md).
  // Fails with kUnavailable when OIDN cannot create it, with OIDN's cause.
  [[nodiscard]] static std::expected<Device, core::Error> CreateCpu();

  // OIDN's CUDA device on the GPU of CUDA ordinal `ordinal`, on its default
  // stream, committed. Fails as CreateCpu does.
  [[nodiscard]] static std::expected<Device, core::Error> CreateCuda(
      int ordinal);

  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;
  Device(Device&& other) noexcept;
  Device& operator=(Device&& other) noexcept;
  ~Device();

  // Fails with the error OIDN reported for the device, or for what was made
  // through it, since it was last asked, as the failure to `what`:
  // "Open Image Denoise: cannot <what>: <message>". kUnavailable when memory
  // ran out or the hardware is not supported, kInternal otherwise.
  [[nodiscard]] std::expected<void, core::Error> Check(
      std::string_view what) const;

  // The threads of a CPU device.
  [[nodiscard]] int threads() const;

  // OIDN's handle, which it owns: the bridge's own type.
  struct Handle;

 private:
  friend class Buffer;
  friend class Filter;

  explicit Device(std::unique_ptr<Handle> handle);

  std::unique_ptr<Handle> handle_;
};

// Bytes in the memory of a device, released when it is destroyed. Its
// device reports the errors of its creation, writes and reads.
class UMBRA_OWNER Buffer {
 public:
  // No buffer.
  Buffer();

  // `bytes` bytes, uninitialized, in the memory of `device`; none when the
  // device cannot allocate them.
  [[nodiscard]] static Buffer Create(const Device& device, std::size_t bytes);

  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;
  Buffer(Buffer&& other) noexcept;
  Buffer& operator=(Buffer&& other) noexcept;
  ~Buffer();

  // Whether it holds no buffer.
  [[nodiscard]] bool IsEmpty() const { return handle_ == nullptr; }

  // Copies `values` to the start of the buffer, and waits until they are.
  // NOLINTNEXTLINE(readability-make-member-function-const): writes its memory.
  void Write(std::span<const float> values);

  // Copies the start of the buffer into `values`, and waits until it is.
  void Read(std::span<float> values) const;

  // OIDN's handle, which it owns: the bridge's own type.
  struct Handle;

 private:
  friend class Filter;

  explicit Buffer(std::unique_ptr<Handle> UMBRA_NULLABLE handle);

  std::unique_ptr<Handle> UMBRA_NULLABLE handle_;
};

// The format of a pixel of a filter's image.
enum class ImageFormat : std::uint8_t {
  // Three float32: RGB, or XYZ.
  kFloat3,
};

// The quality of a filter, against its speed.
enum class Quality : std::uint8_t {
  kDefault,
  kFast,
  kBalanced,
  kHigh,
};

// An image of a filter: `width` × `height` pixels of `format`, row by row,
// from the start of `buffer`, `pixel_stride` bytes apart, or packed for 0;
// none when `buffer` is null.
struct Image {
  const Buffer* UMBRA_NULLABLE buffer = nullptr;
  ImageFormat format = ImageFormat::kFloat3;
  std::size_t width = 0;
  std::size_t height = 0;
  std::size_t pixel_stride = 0;
};

// What a filter calls, on OIDN's threads, as it progresses: `user` is what
// SetProgressMonitor was given, `fraction` the share of its work done, from
// 0 to 1. It returns whether the filter goes on. It must neither allocate
// nor lock.
using ProgressMonitor = bool (*)(void* UMBRA_NULLABLE user, double fraction)
    [[clang::nonblocking]];

// A filter on a device, released when it is destroyed. Its device reports
// the errors of its creation, its parameters, its commit and its execution.
// A parameter or an image set takes effect at the next Commit.
class UMBRA_OWNER Filter {
 public:
  // No filter.
  Filter();

  // A filter of OIDN's type `type` ("RT") on `device`; none when the device
  // cannot create it.
  [[nodiscard]] static Filter Create(const Device& device, const char* type);

  Filter(const Filter&) = delete;
  Filter& operator=(const Filter&) = delete;
  Filter(Filter&& other) noexcept;
  Filter& operator=(Filter&& other) noexcept;
  ~Filter();

  // Sets the bool parameter `name` ("hdr") to true.
  // NOLINTBEGIN(readability-make-member-function-const): they change the
  // filter.
  void Enable(const char* name);

  // Sets the filter's quality.
  void SetQuality(Quality quality);

  // Calls `monitor` with `user` as it progresses.
  void SetProgressMonitor(ProgressMonitor monitor, void* UMBRA_NULLABLE user);

  // Binds the image `name` ("color", "output") to `image`, whose buffer
  // outlives the binding.
  void SetImage(const char* name, const Image& image);

  // Commits the parameters and images set since the last commit.
  void Commit();

  // Runs the filter, and waits until it is done.
  void Execute();
  // NOLINTEND(readability-make-member-function-const)

  // OIDN's handle, which it owns: the bridge's own type.
  struct Handle;

 private:
  explicit Filter(std::unique_ptr<Handle> UMBRA_NULLABLE handle);

  std::unique_ptr<Handle> UMBRA_NULLABLE handle_;
};

}  // namespace umbra::ext::oidn

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_OIDN_OWNERS_H_
