// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// What a GPU holds of the prims committed to it, in the tables by id of
// umbra/device/id_table.h, whose statuses it turns into messages
// (docs/adr/0013-devices-render-from-their-own-memory.md).

#ifndef UMBRA_DEVICE_GPU_BACKEND_HELD_H_
#define UMBRA_DEVICE_GPU_BACKEND_HELD_H_

#include <driver_types.h>

#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "support.h"
#include "umbra/core/safety.h"
#include "umbra/device/id_table.h"
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

// The message of `error`, an id of the prims `kind` ("prototype").
[[nodiscard]] std::string MessageOf(const device::IdError& error,
                                    std::string_view kind);

// Applies the ids of `changes` to `table`, of `count` ids of the prims
// `kind`, as device::ApplyIds does: fails with the message of the first id
// that does not apply.
template <typename Changes, typename T, typename Id>
[[nodiscard]] Result<void> ApplyIds(const Changes& changes, std::uint32_t count,
                                    device::IdTable<T, Id>& table,
                                    std::string_view kind) {
  return device::ApplyIds(changes, count, table)
      .transform_error(
          [&](const device::IdError& error) { return MessageOf(error, kind); });
}

// What `table`, of the prims `kind`, holds under `key`.
template <typename T, typename Id>
[[nodiscard]] Result<T*> HeldIn(device::IdTable<T, Id>& table
                                    UMBRA_LIFETIMEBOUND,
                                Id key, std::string_view kind) {
  T* UMBRA_NULLABLE const held = table.Find(key);
  if (held == nullptr) {
    return std::unexpected(
        std::format("{} {} is not held", kind, std::to_underlying(key)));
  }
  T& value = *held;
  return &value;
}

}  // namespace umbra::device::gpu::backend

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_GPU_BACKEND_HELD_H_
