// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// What a GPU holds of the prims committed to it, in tables indexed by id
// (docs/adr/0013-devices-render-from-their-own-memory.md).

#ifndef UMBRA_DEVICE_GPU_BACKEND_HELD_H_
#define UMBRA_DEVICE_GPU_BACKEND_HELD_H_

#include <driver_types.h>

#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "support.h"
#include "umbra/core/safety.h"
#include "umbra/device/render_data.h"
#include "umbra/ext/cuda/owners.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device::gpu::backend {

// A primvar in a GPU's memory: its values and, per corner, their indices.
struct HeldPrimvar {
  std::string name;
  device::PrimvarRate rate = device::PrimvarRate::kConstant;
  std::uint32_t components = 1;
  ext::cuda::DeviceBuffer floats;
  ext::cuda::DeviceBuffer integers;
  ext::cuda::DeviceBuffer indices;
};

// Uploads `primvar` on `stream`.
[[nodiscard]] Result<HeldPrimvar> UploadPrimvar(const device::Primvar& primvar,
                                                cudaStream_t stream);

// The 3x4 rows of `matrix` for column vectors, as OptiX takes a transform:
// `matrix` is for row vectors.
[[nodiscard]] std::array<float, 12> RowsOf(const device::Matrix& matrix);

// Where each instance of a prim is in a GPU's memory: the first key and the
// number of keys of each, and every key, as RowsOf gives it.
struct HeldPlacements {
  ext::cuda::DeviceBuffer ranges;
  ext::cuda::DeviceBuffer keys;
  std::uint32_t count = 0;
};

// The first key of an instance in HeldPlacements::keys, and its number of
// keys.
struct KeyRange {
  std::uint32_t first = 0;
  std::uint32_t count = 0;
};

// Uploads the transforms of `instances` on `stream`.
[[nodiscard]] Result<HeldPlacements> UploadPlacements(
    std::span<const device::Instance> instances, cudaStream_t stream);

// The prims of one kind a GPU holds, by id.
template <typename T, typename Id>
class IdTable {
 public:
  // `kind` names the prims in messages ("prototype").
  explicit IdTable(std::string_view kind) : kind_(kind) {}

  // Makes room for every id below `count`.
  void Reserve(std::uint32_t count) {
    if (count > slots_.size()) {
      slots_.resize(count);
    }
  }

  // Holds `value` under `key`, which must be free.
  [[nodiscard]] Result<void> Add(Id key, T value) {
    auto slot = Slot(key);
    if (!slot.has_value()) {
      return std::unexpected(slot.error());
    }
    std::optional<T>& held = **slot;
    if (held.has_value()) {
      return std::unexpected(
          std::format("{} {} is already held", kind_, std::to_underlying(key)));
    }
    held.emplace(std::move(value));
    return {};
  }

  // What is held under `key`.
  [[nodiscard]] Result<T*> Held(Id key) UMBRA_LIFETIMEBOUND {
    auto slot = Slot(key);
    if (!slot.has_value()) {
      return std::unexpected(slot.error());
    }
    std::optional<T>& held = **slot;
    if (!held.has_value()) {
      return std::unexpected(
          std::format("{} {} is not held", kind_, std::to_underlying(key)));
    }
    return &*held;
  }

  // Drops what is held under `key`.
  [[nodiscard]] Result<void> Remove(Id key) {
    return Held(key).transform(
        [&](T* /*held*/) { slots_.at(std::to_underlying(key)).reset(); });
  }

  // Calls `visit` with each held value, by id.
  template <typename Visit>
  void ForEach(const Visit& visit) const {
    for (const std::optional<T>& slot : slots_) {
      if (slot.has_value()) {
        visit(*slot);
      }
    }
  }

  // Drops everything.
  void Clear() { slots_.clear(); }

 private:
  Result<std::optional<T>*> Slot(Id key) UMBRA_LIFETIMEBOUND {
    const auto index = std::to_underlying(key);
    if (index >= slots_.size()) {
      return std::unexpected(std::format("{} {} is out of range ({} ids)",
                                         kind_, index, slots_.size()));
    }
    return &slots_.at(index);
  }

  std::string_view kind_;
  std::vector<std::optional<T>> slots_;
};

}  // namespace umbra::device::gpu::backend

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_GPU_BACKEND_HELD_H_
