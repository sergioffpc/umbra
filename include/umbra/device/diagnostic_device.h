// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The diagnostic device: a device that renders nothing, and checks each
// change set it receives instead.

#ifndef UMBRA_DEVICE_DIAGNOSTIC_DEVICE_H_
#define UMBRA_DEVICE_DIAGNOSTIC_DEVICE_H_

#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/device/change_set.h"
#include "umbra/device/device.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device {

// A device that keeps, of each prim it holds, its path, the sizes its
// updates are checked against, its references to other prims, and a digest
// of each of its parts. At each commit it applies the change set to that
// state and checks it: every id below its kind's count, added only when it
// does not hold it, updated or removed only when it does; every reference to
// a prim of another kind resolving to one it holds once the commit's
// additions are applied and its removals are not; the sizes of every array
// (motion samples as long as each other, indices in range, primvars as many
// as their rate asks), those of an updated prototype against the parts it
// keeps; an updated prototype giving only the members its flags name; and
// every numeric array aligned to kBufferAlignment. It logs what it received
// at debug level, as the device component, each prim updated or removed
// with it, and then the state it holds, with a digest of it that does not
// depend on the ids: two devices hold the same prims, with the same values,
// when their digests are equal.
class DiagnosticDevice final : public Device {
 public:
  DiagnosticDevice();
  DiagnosticDevice(const DiagnosticDevice&) = delete;
  DiagnosticDevice& operator=(const DiagnosticDevice&) = delete;
  DiagnosticDevice(DiagnosticDevice&&) = delete;
  DiagnosticDevice& operator=(DiagnosticDevice&&) = delete;
  ~DiagnosticDevice() override;

  [[nodiscard]] std::string_view name() const override;

  // Fails with kInternal, naming the first inconsistency, and then holds
  // what it held before.
  [[nodiscard]] std::expected<void, core::Error> Commit(
      const ChangeSet& changes) override;

  // What it keeps of the prims it holds.
  struct State;

 private:
  std::unique_ptr<State> state_;
  std::uint64_t commits_ = 0;
};

}  // namespace umbra::device

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_DIAGNOSTIC_DEVICE_H_
