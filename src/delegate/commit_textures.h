// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The textures a commit gives the devices: those the materials and the
// lights read, each once.

#ifndef UMBRA_DELEGATE_COMMIT_TEXTURES_H_
#define UMBRA_DELEGATE_COMMIT_TEXTURES_H_

#include <vector>

#include "host_copy.h"
#include "ledger.h"
#include "umbra/core/safety.h"
#include "views.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::delegate::internal {

// The textures the materials and the lights read: each added when the
// first reads it, and removed when the last stops.
class Textures {
 public:
  Textures(HeldTextures& held UMBRA_LIFETIMEBOUND,
           ChangeSetBuilder& changes UMBRA_LIFETIMEBOUND)
      : held_(&held), changes_(&changes) {}

  // Makes `read` the textures a material or a light reads, which `held`
  // were: counts a reader of each of `read`, then one less of each of
  // `held`. The change set can then refer to `read` by their ids.
  void Swap(std::vector<host::Texture>& held, std::vector<host::Texture> read);
  // Counts a reader less of each of `textures`.
  void Unread(const std::vector<host::Texture>& textures);

 private:
  // Counts a reader of each of `textures`.
  void Read(const std::vector<host::Texture>& textures);

  HeldTextures* held_;
  ChangeSetBuilder* changes_;
};

}  // namespace umbra::delegate::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DELEGATE_COMMIT_TEXTURES_H_
