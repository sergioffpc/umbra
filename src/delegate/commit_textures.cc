// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "commit_textures.h"

#include <utility>
#include <vector>

#include "host_copy.h"
#include "ledger.h"

namespace umbra::delegate::internal {

void Textures::Swap(std::vector<host::Texture>& held,
                    std::vector<host::Texture> read) {
  Read(read);
  Unread(std::exchange(held, std::move(read)));
}

void Textures::Read(const std::vector<host::Texture>& textures) {
  for (const host::Texture& texture : textures) {
    const auto [held, added] = held_->Hold(texture);
    ++held.second.readers;
    if (added) {
      // The change set views the ledger's copy, which outlives the commit.
      changes_->AddTexture(held.second.id, held.first);
    }
  }
}

void Textures::Unread(const std::vector<host::Texture>& textures) {
  for (const host::Texture& texture : textures) {
    HeldTexture* held = held_->Find(texture);
    if (held == nullptr || --held->readers > 0) {
      continue;
    }
    held_->Remove(texture, [this](const HeldTexture& removed) {
      changes_->RemoveTexture(removed.id);
    });
  }
}

}  // namespace umbra::delegate::internal
