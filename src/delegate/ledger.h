// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The ledger: what the devices hold after a commit, which the next one
// starts from, so that it gives them only what changed
// (docs/adr/0013-devices-render-from-their-own-memory.md). Each kind of
// prim is a held table, by key, whose ids are dense and reused once freed.

#ifndef UMBRA_DELEGATE_LEDGER_H_
#define UMBRA_DELEGATE_LEDGER_H_

#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "host_copy.h"
#include "umbra/core/safety.h"
#include "umbra/device/change_set.h"
#include "umbra/device/render_data.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::delegate::internal {

namespace pxr = PXR_NS;

// The ids of one kind, dense: an id freed is taken again, but not before
// the next commit.
template <typename Id>
class IdPool {
 public:
  // A free id, or the next one.
  [[nodiscard]] Id Take() {
    if (!free_.empty()) {
      const Id taken = free_.back();
      free_.pop_back();
      return taken;
    }
    return static_cast<Id>(count_++);
  }

  // Frees `freed`, for a later commit.
  void Free(Id freed) { freed_.push_back(freed); }

  // Makes the ids freed by this commit free for the next.
  void Recycle() {
    free_.insert(free_.end(), freed_.begin(), freed_.end());
    freed_.clear();
  }

  // The number of ids: every id taken is below it.
  [[nodiscard]] std::uint32_t count() const { return count_; }

 private:
  std::uint32_t count_ = 0;
  std::vector<Id> free_;
  std::vector<Id> freed_;
};

// The prims of one kind the devices hold, by key: each `Held` has the id
// taken when it was added, `Held::id`, which is freed when it is removed.
template <typename Key, typename Held>
class HeldTable {
 public:
  using Id = decltype(Held::id);
  using Entry = std::pair<const Key, Held>;

  // The prim held at `key`, or null.
  [[nodiscard]] Held* UMBRA_NULLABLE Find(const Key& key) UMBRA_LIFETIMEBOUND {
    const auto found = held_.find(key);
    return found == held_.end() ? nullptr : &found->second;
  }
  [[nodiscard]] const Held* UMBRA_NULLABLE
  Find(const Key& key) const UMBRA_LIFETIMEBOUND {
    const auto found = held_.find(key);
    return found == held_.end() ? nullptr : &found->second;
  }

  // The prim held at `key`, which must be.
  [[nodiscard]] Held& at(const Key& key) UMBRA_LIFETIMEBOUND {
    return held_.at(key);
  }
  [[nodiscard]] const Held& at(const Key& key) const UMBRA_LIFETIMEBOUND {
    return held_.at(key);
  }

  // The prim at `key`, added with an id of its own when it is not held, and
  // whether it was: the commit then adds it, and otherwise updates it.
  std::pair<Entry&, bool> Hold(const Key& key) UMBRA_LIFETIMEBOUND {
    const auto [held, added] = held_.try_emplace(key);
    if (added) {
      held->second.id = ids_.Take();
    }
    return {*held, added};
  }

  // Removes the prim held at `key`, if any: `release(held)`, then frees its
  // id.
  template <typename Release>
  void Remove(const Key& key, Release release) {
    const auto held = held_.find(key);
    if (held == held_.end()) {
      return;
    }
    release(std::as_const(held->second));
    ids_.Free(held->second.id);
    held_.erase(held);
  }

  // Removes, as Remove does, every prim held for which `gone(key, held)`.
  template <typename Gone, typename Release>
  void RemoveIf(Gone gone, Release release) {
    std::erase_if(held_, [&](const Entry& entry) {
      if (!gone(entry.first, entry.second)) {
        return false;
      }
      release(entry.second);
      ids_.Free(entry.second.id);
      return true;
    });
  }

  // Removes, as Remove does, every prim held whose key `present` does not
  // contain.
  template <typename Present, typename Release>
  void RemoveMissing(const Present& present, Release release) {
    RemoveIf(
        [&present](const Key& key, const Held& /*held*/) {
          return !present.contains(key);
        },
        release);
  }

  [[nodiscard]] auto begin() const UMBRA_LIFETIMEBOUND { return held_.begin(); }
  [[nodiscard]] auto end() const UMBRA_LIFETIMEBOUND { return held_.end(); }

  // The number of ids: every id taken is below it.
  [[nodiscard]] std::uint32_t id_count() const { return ids_.count(); }

  // Makes the ids the commit freed free for the next.
  void Recycle() { ids_.Recycle(); }

 private:
  std::map<Key, Held> held_;
  IdPool<Id> ids_;
};

// A camera the devices hold.
struct HeldCamera {
  device::CameraId id{};
};

// A texture the devices hold: its id, and the materials and lights that
// read it.
struct HeldTexture {
  device::TextureId id{};
  std::size_t readers = 0;
};

// A prototype the devices hold: its id, and the keys of its materials.
struct HeldPrototype {
  device::PrototypeId id{};
  std::vector<std::string> materials;
};

// A material the devices hold: its id, the textures it reads, and the
// prototypes that read it.
struct HeldMaterial {
  device::MaterialId id{};
  std::vector<host::Texture> textures;
  std::size_t readers = 0;
};

// A light the devices hold: its id, and the textures it reads.
struct HeldLight {
  device::LightId id{};
  std::vector<host::Texture> textures;
};

// The textures held, by file and color space.
using HeldTextures = HeldTable<host::Texture, HeldTexture>;

// What the devices hold after a commit, which the next one starts from: each
// prim by its path, and what it reads. A material is held by the path of its
// prim, or by the name of one of umbra's.
struct Ledger {
  HeldTable<pxr::SdfPath, HeldPrototype> prototypes;
  HeldTable<pxr::SdfPath, HeldLight> lights;
  HeldTable<std::string, HeldMaterial> materials;
  HeldTable<pxr::SdfPath, HeldCamera> cameras;
  HeldTextures textures;
  // The shutter the prims were sampled over; none before the first commit.
  std::optional<host::Shutter> shutter;
  // The warnings logged: a later commit does not log them again.
  std::set<std::string> warnings;

  // The number of ids of each kind.
  [[nodiscard]] device::IdCounts id_counts() const {
    return {
        .prototypes = prototypes.id_count(),
        .lights = lights.id_count(),
        .materials = materials.id_count(),
        .cameras = cameras.id_count(),
        .textures = textures.id_count(),
    };
  }

  // Makes the ids the commit freed free for the next.
  void Recycle() {
    prototypes.Recycle();
    lights.Recycle();
    materials.Recycle();
    cameras.Recycle();
    textures.Recycle();
  }
};

}  // namespace umbra::delegate::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DELEGATE_LEDGER_H_
