// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The diagnostic device: a device that renders nothing, and checks each
// change set it receives instead.

#ifndef UMBRA_DEVICE_DIAGNOSTIC_DEVICE_H_
#define UMBRA_DEVICE_DIAGNOSTIC_DEVICE_H_

#include <cstdint>
#include <expected>
#include <string_view>
#include <vector>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/device/change_set.h"
#include "umbra/device/device.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device {

// A device that keeps only which ids it holds. At each commit it checks the
// change set: every id below its kind's count, added only when it does not
// hold it, updated or removed only when it does; every reference to a prim
// of another kind resolving to one it holds once the commit's additions
// are applied and its removals are not; the sizes of every array (motion
// samples as long as each other, indices in range, primvars as many as
// their rate asks); and every numeric array aligned to kBufferAlignment.
// It logs what it received at debug level, as the device component.
class DiagnosticDevice final : public Device {
 public:
  [[nodiscard]] std::string_view name() const override;

  // Fails with kInternal, naming the first inconsistency, and then holds
  // what it held before.
  [[nodiscard]] std::expected<void, core::Error> Commit(
      const ChangeSet& changes) override;

 private:
  // Which ids of a kind it holds: 1 for each held, by id.
  using Held = std::vector<std::uint8_t>;

  Held prototypes_;
  Held lights_;
  Held materials_;
  Held cameras_;
  Held textures_;
  std::uint64_t commits_ = 0;
};

}  // namespace umbra::device

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_DIAGNOSTIC_DEVICE_H_
