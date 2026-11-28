// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The interface every device implements
// (docs/adr/0013-devices-render-from-their-own-memory.md), the frame
// parameters a device receives for each render product besides its render
// data, and the counter devices take the product's tiles from
// (docs/adr/0014-frames-render-in-tiles-from-one-counter.md).

#ifndef UMBRA_DEVICE_DEVICE_H_
#define UMBRA_DEVICE_DEVICE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <new>
#include <span>
#include <string_view>
#include <vector>

#include "umbra/core/error.h"
#include "umbra/core/progress.h"
#include "umbra/core/safety.h"
#include "umbra/device/change_set.h"
#include "umbra/device/image.h"
#include "umbra/device/render_data.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device {

// The interval a frame integrates, in frames relative to the frame: when the
// shutter opens and when it closes. Both are 0 when there is no motion blur.
// The motion samples of a prim that moves are equally spaced over it, the
// first when it opens and the last when it closes; a prim that does not move
// has one sample, at the frame.
struct Shutter {
  double open = 0.0;
  double close = 0.0;

  friend bool operator==(const Shutter&, const Shutter&) = default;
};

// A render var a device renders, and the render buffer it fills.
struct FilmVar {
  VarSource source = VarSource::kColor;
  // The render buffer's pixels, float32: `channels` values per pixel of the
  // data window, the pixels row by row from its top left.
  std::span<float> pixels;
  std::uint32_t channels = 1;
};

// What a device renders a render product with, besides its render data.
struct FrameParameters {
  Shutter shutter;
  // The camera it sees through.
  CameraId camera{};
  Image image;
  MotionBlur motion_blur = MotionBlur::kEnabled;
  DepthOfField depth_of_field = DepthOfField::kEnabled;
  // The samples each pixel takes: at least 1.
  std::uint32_t samples_per_pixel = 1;
  // The render vars it renders, each into its render buffer.
  std::span<const FilmVar> vars;
};

// The width and height of a tile, in pixels.
inline constexpr std::int32_t kTileSize = 64;

// The tiles of a render product's data window, and the counter devices take
// them from (docs/adr/0014-frames-render-in-tiles-from-one-counter.md): the
// window cut into tiles of kTileSize pixels, smaller on its right and bottom
// edges, numbered row by row from its top left. Devices take tiles in
// batches, one atomic add per batch, until none is left, and finish each
// tile once its pixels are in the render buffers, which counts it into the
// product's progress.
class TileQueue {
 public:
  // The first tile of a batch, and the tile after its last.
  struct Batch {
    std::uint64_t first = 0;
    std::uint64_t end = 0;

    [[nodiscard]] bool IsEmpty() const { return end <= first; }
  };

  // The queue of the tiles of `window`, whose devices count the tiles they
  // finish into `progress`, if any.
  //
  // Each device copies each tile it renders into the render buffers as soon
  // as the tile is rendered, then calls Finish: a tile IsFinished once its
  // pixels in the render buffers are final, which a display may read while
  // the other tiles render
  // (docs/adr/0015-tiles-reach-the-render-buffers-as-they-finish.md).
  explicit TileQueue(const PixelWindow& window,
                     core::ProgressPhase* UMBRA_NULLABLE progress = nullptr);

  // The number of tiles.
  [[nodiscard]] std::uint64_t count() const { return count_; }

  // The tiles taken: at most count().
  [[nodiscard]] std::uint64_t taken() const;

  // Takes the next `size` tiles, fewer at the end, none once every tile is
  // taken. Callable from any thread.
  [[nodiscard]] Batch Take(std::uint64_t size) [[clang::nonblocking]] {
    const std::uint64_t first =
        next_.fetch_add(size, std::memory_order_relaxed);
    if (first >= count_) {
      return {.first = count_, .end = count_};
    }
    return {
        .first = first,
        .end = first + size < count_ ? first + size : count_,
    };
  }

  // The pixels of tile `tile`. Callable on the hot path: it neither
  // allocates nor locks.
  [[nodiscard]] PixelWindow TileWindow(std::uint64_t tile) const
      [[clang::nonblocking]];

  // Marks tile `tile` finished, its pixels in the render buffers final, and
  // counts it into the progress. Callable from any thread, once per tile, on
  // the hot path: it neither allocates nor locks.
  void Finish(std::uint64_t tile) [[clang::nonblocking]] {
    std::span(finished_).subspan(tile, 1).front().store(
        1, std::memory_order_release);
    if (progress_ != nullptr) {
      progress_->Add(1);
    }
  }

  // Whether tile `tile` is finished: once it is, the pixels of its window in
  // the render buffers are final, and the thread that saw it may read them.
  [[nodiscard]] bool IsFinished(std::uint64_t tile) const {
    return std::span(finished_).subspan(tile, 1).front().load(
               std::memory_order_acquire) != 0;
  }

 private:
  // The next tile to take, on a cache line of its own.
  alignas(std::hardware_destructive_interference_size)
      std::atomic<std::uint64_t> next_{0};
  alignas(std::hardware_destructive_interference_size) PixelWindow window_;
  std::uint64_t columns_ = 0;
  std::uint64_t count_ = 0;
  core::ProgressPhase* UMBRA_NULLABLE progress_;
  // Whether each tile is finished: 1 once it is.
  std::vector<std::atomic<std::uint8_t>> finished_;
};

// A device: what a render runs on, which renders from its own memory only.
class Device {
 public:
  Device() = default;
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;
  Device(Device&&) = delete;
  Device& operator=(Device&&) = delete;
  virtual ~Device();

  // Its name ("cpu0", "gpu0").
  [[nodiscard]] virtual std::string_view name() const UMBRA_LIFETIMEBOUND = 0;

  // Copies what `changes` gives into its own memory and builds or updates
  // its render data from it, dropping what it built from the prims removed.
  // It reads `changes` only during the call. Fails with the cause when it
  // cannot: the render then stops.
  [[nodiscard]] virtual std::expected<void, core::Error> Commit(
      const ChangeSet& changes) = 0;

  // Renders the render product `frame` describes, from what it committed:
  // takes batches of tiles from `tiles` until none is left, renders them
  // into its own film, copies each tile into the render buffers of
  // `frame`'s vars as soon as it is rendered, and finishes it in `tiles`
  // (docs/adr/0015-tiles-reach-the-render-buffers-as-they-finish.md). Other
  // devices take the other tiles at the same time. Fails with the cause when
  // it cannot: the render then stops.
  [[nodiscard]] virtual std::expected<void, core::Error> Render(
      const FrameParameters& frame, TileQueue& tiles) = 0;
};

}  // namespace umbra::device

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_DEVICE_H_
