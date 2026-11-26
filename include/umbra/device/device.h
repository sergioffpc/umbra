// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The interface every device implements
// (docs/adr/0013-devices-render-from-their-own-memory.md), and the frame
// parameters a device receives for each frame besides its render data.

#ifndef UMBRA_DEVICE_DEVICE_H_
#define UMBRA_DEVICE_DEVICE_H_

#include <expected>
#include <string_view>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/device/change_set.h"
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

// What a frame renders besides render data.
struct FrameParameters {
  Shutter shutter;
  // The camera it sees through.
  CameraId camera{};
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
};

}  // namespace umbra::device

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_DEVICE_H_
