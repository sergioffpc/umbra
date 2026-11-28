// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// What a device holds of the prims committed to it, by id, and how the ids
// of a change set apply to it
// (docs/adr/0013-devices-render-from-their-own-memory.md). Every device
// applies a change set's ids by the same rules, and writes its own message
// from the status they return. Header-only: the GPU module, which links no
// umbra module, uses it too.

#ifndef UMBRA_DEVICE_ID_TABLE_H_
#define UMBRA_DEVICE_ID_TABLE_H_

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "umbra/core/safety.h"
#include "umbra/device/camera_rays.h"
#include "umbra/device/change_set.h"
#include "umbra/device/device.h"
#include "umbra/device/render_data.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device {

// Why an id does not apply to a table.
enum class IdFault : std::uint8_t {
  // It is not below the number of ids of its kind.
  kOutOfRange,
  // It is removed, and not held.
  kRemovedNotHeld,
  // It is updated, and not held, or removed by the same change set.
  kUpdatedNotHeld,
  // It is updated twice by the same change set.
  kUpdatedTwice,
  // It is added, and already held.
  kAddedHeld,
  // It is held, and not below the number of ids it shrinks to.
  kHeldOutOfRange,
};

// The id that does not apply, and why.
struct IdError {
  IdFault fault = IdFault::kOutOfRange;
  std::uint32_t id = 0;
};

// Whether an id, or every id of a change set, applies.
using IdStatus = std::expected<void, IdError>;

// The prims of one kind a device holds, by id: a value of T for each id it
// holds, below size(). A value stays where it is, at the same address,
// until its id is removed, whatever the table does with the other ids.
template <typename T, typename Id>
class IdTable {
 public:
  IdTable() = default;
  IdTable(const IdTable& other)
    requires std::copy_constructible<T>
      : slots_(other.slots_.size()) {
    for (std::size_t index = 0; index < slots_.size(); ++index) {
      if (const std::unique_ptr<T>& held = other.slots_.at(index);
          held != nullptr) {
        slots_.at(index) = std::make_unique<T>(*held);
      }
    }
  }
  IdTable& operator=(const IdTable& other)
    requires std::copy_constructible<T>
  {
    if (this != &other) {
      IdTable copy(other);
      slots_ = std::move(copy.slots_);
    }
    return *this;
  }
  IdTable(IdTable&&) noexcept = default;
  IdTable& operator=(IdTable&&) noexcept = default;
  ~IdTable() = default;

  // The number of ids it has room for: every id it holds is below it.
  [[nodiscard]] std::uint32_t size() const {
    return static_cast<std::uint32_t>(slots_.size());
  }

  // Makes room for every id below `count`. It never shrinks.
  void Reserve(std::uint32_t count) {
    if (count > slots_.size()) {
      slots_.resize(count);
    }
  }

  // Whether it holds `key`.
  [[nodiscard]] bool Contains(Id key) const { return Find(key) != nullptr; }

  // What it holds under `key`; null when it holds nothing there.
  [[nodiscard]] T* UMBRA_NULLABLE Find(Id key) UMBRA_LIFETIMEBOUND {
    const auto index = std::to_underlying(key);
    return index < slots_.size() ? slots_.at(index).get() : nullptr;
  }
  [[nodiscard]] const T* UMBRA_NULLABLE Find(Id key) const UMBRA_LIFETIMEBOUND {
    const auto index = std::to_underlying(key);
    return index < slots_.size() ? slots_.at(index).get() : nullptr;
  }

  // Holds `value` under `key`, which must be below size() and free.
  [[nodiscard]] IdStatus Add(Id key, T value) {
    const auto index = std::to_underlying(key);
    if (index >= slots_.size()) {
      return std::unexpected(
          IdError{.fault = IdFault::kOutOfRange, .id = index});
    }
    std::unique_ptr<T>& slot = slots_.at(index);
    if (slot != nullptr) {
      return std::unexpected(
          IdError{.fault = IdFault::kAddedHeld, .id = index});
    }
    slot = std::make_unique<T>(std::move(value));
    return {};
  }

  // Drops what it holds under `key`, which must be held.
  [[nodiscard]] IdStatus Remove(Id key) {
    const auto index = std::to_underlying(key);
    if (index >= slots_.size()) {
      return std::unexpected(
          IdError{.fault = IdFault::kOutOfRange, .id = index});
    }
    std::unique_ptr<T>& slot = slots_.at(index);
    if (slot == nullptr) {
      return std::unexpected(
          IdError{.fault = IdFault::kRemovedNotHeld, .id = index});
    }
    slot.reset();
    return {};
  }

  // Makes room for the ids below `count` only, when it holds none of the
  // others; keeps what it holds otherwise.
  [[nodiscard]] IdStatus Shrink(std::uint32_t count) {
    for (std::size_t index = count; index < slots_.size(); ++index) {
      if (slots_.at(index) != nullptr) {
        return std::unexpected(IdError{
            .fault = IdFault::kHeldOutOfRange,
            .id = static_cast<std::uint32_t>(index),
        });
      }
    }
    if (count < slots_.size()) {
      slots_.resize(count);
    }
    return {};
  }

  // Calls `visit` with each id it holds and its value, by id.
  template <typename Visit>
  void ForEach(const Visit& visit) const {
    for (std::size_t index = 0; index < slots_.size(); ++index) {
      if (const std::unique_ptr<T>& held = slots_.at(index); held != nullptr) {
        visit(Id{static_cast<std::uint32_t>(index)}, *held);
      }
    }
  }

  // Drops everything, and the room for every id.
  void Clear() { slots_.clear(); }

 private:
  std::vector<std::unique_ptr<T>> slots_;
};

// The id of a prim a change set adds, updates or removes.
template <typename View>
[[nodiscard]] constexpr auto IdOf(const View& view) {
  return view.id;
}
[[nodiscard]] constexpr PrototypeId IdOf(const UpdatedPrototype& updated) {
  return updated.prototype.id;
}

namespace internal {

// Fails when `key` is not below `count`.
inline IdStatus CheckBelow(std::uint32_t key, std::uint32_t count) {
  if (key >= count) {
    return std::unexpected(IdError{.fault = IdFault::kOutOfRange, .id = key});
  }
  return {};
}

// Drops the ids `removed` from `table`, which must hold them.
template <typename T, typename Id>
IdStatus RemoveIds(std::span<const Id> removed, std::uint32_t count,
                   IdTable<T, Id>& table) {
  for (const Id key : removed) {
    if (auto done = CheckBelow(std::to_underlying(key), count).and_then([&] {
          return table.Remove(key);
        });
        !done.has_value()) {
      return done;
    }
  }
  return {};
}

// Checks that `table` holds each id of `updated`, and that none comes
// twice.
template <typename View, typename T, typename Id>
IdStatus CheckUpdated(std::span<const View> updated, std::uint32_t count,
                      const IdTable<T, Id>& table) {
  std::vector<std::uint32_t> ids;
  ids.reserve(updated.size());
  for (const View& view : updated) {
    const Id key = IdOf(view);
    const auto index = std::to_underlying(key);
    if (auto below = CheckBelow(index, count); !below.has_value()) {
      return below;
    }
    if (!table.Contains(key)) {
      return std::unexpected(
          IdError{.fault = IdFault::kUpdatedNotHeld, .id = index});
    }
    ids.push_back(index);
  }
  std::ranges::sort(ids);
  if (const auto twice = std::ranges::adjacent_find(ids); twice != ids.end()) {
    return std::unexpected(
        IdError{.fault = IdFault::kUpdatedTwice, .id = *twice});
  }
  return {};
}

// Holds each id of `added` in `table`, which must not hold it yet, with a
// value of T made anew.
template <typename View, typename T, typename Id>
IdStatus AddIds(std::span<const View> added, std::uint32_t count,
                IdTable<T, Id>& table) {
  for (const View& view : added) {
    const Id key = IdOf(view);
    if (auto done = CheckBelow(std::to_underlying(key), count).and_then([&] {
          return table.Add(key, T());
        });
        !done.has_value()) {
      return done;
    }
  }
  return {};
}

}  // namespace internal

// Applies the ids of `changes`, the changes of one kind of prim, to
// `table`, which makes room for the `count` ids of the kind: drops the ids
// removed, checks that it holds each id updated, once, then holds each id
// added, which must be free, with a value of T made anew, which the device
// then fills as it fills the values of the ids updated. Every id must be
// below `count`. It stops at the first id that does not apply, and keeps
// what it applied before it.
template <typename Changes, typename T, typename Id>
[[nodiscard]] IdStatus ApplyIds(const Changes& changes, std::uint32_t count,
                                IdTable<T, Id>& table) {
  table.Reserve(count);
  return internal::RemoveIds(changes.removed, count, table)
      .and_then(
          [&] { return internal::CheckUpdated(changes.updated, count, table); })
      .and_then([&] { return internal::AddIds(changes.added, count, table); });
}

// The camera model of the render product `frame` describes, from its
// camera in `cameras`, whose camera `view_of` gives of the value held:
// none when it holds no such camera.
template <typename T, typename ViewOf>
[[nodiscard]] std::optional<CameraModel> FrameCameraModel(
    const IdTable<T, CameraId>& cameras, const FrameParameters& frame,
    const ViewOf& view_of) {
  const T* UMBRA_NULLABLE const held = cameras.Find(frame.camera);
  if (held == nullptr) {
    return std::nullopt;
  }
  return MakeCameraModel(view_of(*held), frame);
}

}  // namespace umbra::device

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_ID_TABLE_H_
