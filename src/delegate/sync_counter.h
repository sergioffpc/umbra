// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// What the prims of the render delegate count their syncs into: the phase
// of the frame being synced. The prims live for the whole render, and each
// frame syncs in a phase of its own.

#ifndef UMBRA_DELEGATE_SYNC_COUNTER_H_
#define UMBRA_DELEGATE_SYNC_COUNTER_H_

#include <cstdint>

#include "umbra/core/progress.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::delegate::internal {

// Counts syncs into the phase it is given, from any thread; counts nothing
// before it is given one.
class SyncCounter {
 public:
  // Counts the syncs that follow into `phase`, until another is given. Not
  // to be called while prims sync.
  void Begin(core::ProgressPhase& phase UMBRA_LIFETIME_CAPTURE_BY_THIS) {
    phase_ = &phase;
  }

  // Counts nothing more, until another phase is given.
  void End() { phase_ = nullptr; }

  // Adds `count` syncs to the phase.
  void Add(std::uint64_t count) {
    if (phase_ != nullptr) {
      phase_->Add(count);
    }
  }

 private:
  core::ProgressPhase* UMBRA_NULLABLE phase_ = nullptr;
};

}  // namespace umbra::delegate::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DELEGATE_SYNC_COUNTER_H_
