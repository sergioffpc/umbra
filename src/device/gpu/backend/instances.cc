// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "instances.h"

#include <driver_types.h>
#include <optix_types.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <span>
#include <vector>

#include "accel.h"
#include "held.h"
#include "support.h"
#include "umbra/device/render_data.h"
#include "umbra/ext/cuda/conversions.h"
#include "umbra/ext/cuda/owners.h"
#include "umbra/ext/cuda/runtime.h"
#include "umbra/ext/optix/host.h"

namespace umbra::device::gpu::backend {

using ext::cuda::DeviceBuffer;
using ext::cuda::Replace;

namespace {

// The identity, for an instance that gives no transform.
constexpr device::Matrix kIdentity = {
    1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
    0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0,
};

// The record of `kind` placed by `matrix` that points to `handle`.
OptixInstance RecordOf(const device::Matrix& matrix, RecordKind kind,
                       OptixTraversableHandle handle) {
  OptixInstance record{};
  std::ranges::copy(RowsOf(matrix), std::begin(record.transform));
  record.instanceId = kind.prim;
  record.sbtOffset = kind.sbt_offset;
  record.visibilityMask = kind.mask;
  record.flags = OPTIX_INSTANCE_FLAG_NONE;
  record.traversableHandle = handle;
  return record;
}

// Whether `instance` moves within the shutter.
bool IsMoving(const device::Instance& instance) {
  return instance.object_to_world.size() > 1;
}

// Writes `value` into `out` at `offset`.
template <typename T>
void Put(std::span<std::byte> out, std::size_t offset, const T& value) {
  const auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
  std::ranges::copy(bytes, out.subspan(offset, sizeof(T)).begin());
}

// The offset of the keys in an OptixMatrixMotionTransform.
constexpr std::size_t kKeysOffset =
    offsetof(OptixMatrixMotionTransform, transform);
// The bytes of a key.
constexpr std::size_t kKeyBytes = 12 * sizeof(float);

// The bytes from one motion transform to the next, for `moving`.
std::size_t TransformStride(std::span<const device::Instance* const> moving) {
  std::size_t keys = 0;
  for (const device::Instance* const instance : moving) {
    keys = std::max(keys, instance->object_to_world.size());
  }
  const std::size_t bytes = kKeysOffset + (keys * kKeyBytes);
  return (bytes + OPTIX_TRANSFORM_BYTE_ALIGNMENT - 1) /
         OPTIX_TRANSFORM_BYTE_ALIGNMENT * OPTIX_TRANSFORM_BYTE_ALIGNMENT;
}

// The motion transforms of `moving`, `stride` bytes apart, each pointing to
// `child`.
std::vector<std::byte> TransformsOf(
    std::span<const device::Instance* const> moving, std::size_t stride,
    OptixTraversableHandle child) {
  std::vector<std::byte> bytes(stride * moving.size());
  std::size_t offset = 0;
  for (const device::Instance* const instance : moving) {
    const auto keys =
        static_cast<std::uint32_t>(instance->object_to_world.size());
    Put(bytes, offset, child);
    Put(bytes, offset + offsetof(OptixMatrixMotionTransform, motionOptions),
        MotionOptions(keys));
    std::size_t key_offset = offset + kKeysOffset;
    for (const device::Matrix& matrix : instance->object_to_world) {
      Put(bytes, key_offset, RowsOf(matrix));
      key_offset += kKeyBytes;
    }
    offset += stride;
  }
  return bytes;
}

// Uploads the records of the still instances.
Result<void> UploadStill(std::span<const device::Instance* const> still,
                         RecordKind kind, OptixTraversableHandle gas,
                         InstanceRecords& records, cudaStream_t stream) {
  std::vector<OptixInstance> uploaded;
  uploaded.reserve(still.size());
  for (const device::Instance* const instance : still) {
    uploaded.push_back(RecordOf(instance->object_to_world.empty()
                                    ? kIdentity
                                    : instance->object_to_world.front(),
                                kind, gas));
  }
  records.still_count = static_cast<std::uint32_t>(uploaded.size());
  return Replace(records.still, std::span<const OptixInstance>(uploaded),
                 stream);
}

// The records that point to the motion transforms of `records`.
Result<std::vector<OptixInstance>> MovingRecords(const InstanceRecords& records,
                                                 RecordKind kind,
                                                 const Gpu& gpu) {
  std::vector<OptixInstance> moving;
  moving.reserve(records.moving_count);
  for (std::uint32_t index = 0; index < records.moving_count; ++index) {
    const auto handle = ext::optix::TraversableHandle(
        gpu.context,
        records.transforms.address() + (index * records.transform_stride),
        OPTIX_TRAVERSABLE_TYPE_MATRIX_MOTION_TRANSFORM,
        "cannot address a motion transform");
    if (!handle.has_value()) {
      return std::unexpected(handle.error());
    }
    moving.push_back(RecordOf(kIdentity, kind, *handle));
  }
  return moving;
}

// Uploads the motion transforms of the moving instances and their records.
Result<void> UploadMoving(std::span<const device::Instance* const> moving,
                          RecordKind kind, OptixTraversableHandle gas,
                          InstanceRecords& records, const Gpu& gpu) {
  records.transform_stride = TransformStride(moving);
  records.moving_count = static_cast<std::uint32_t>(moving.size());
  const std::vector<std::byte> transforms =
      TransformsOf(moving, records.transform_stride, gas);
  if (auto uploaded =
          Replace(records.transforms, std::span<const std::byte>(transforms),
                  gpu.stream);
      !uploaded.has_value()) {
    return uploaded;
  }
  return MovingRecords(records, kind, gpu)
      .and_then([&](const std::vector<OptixInstance>& records_of_moving) {
        return Replace(records.moving,
                       std::span<const OptixInstance>(records_of_moving),
                       gpu.stream);
      });
}

}  // namespace

OrderedInstances Order(std::span<const device::Instance> instances) {
  OrderedInstances ordered;
  for (const device::Instance& instance : instances) {
    (IsMoving(instance) ? ordered.moving : ordered.still).push_back(&instance);
  }
  return ordered;
}

Result<void> UploadRecords(const OrderedInstances& ordered, RecordKind kind,
                           OptixTraversableHandle gas, InstanceRecords& records,
                           const Gpu& gpu) {
  return UploadStill(ordered.still, kind, gas, records, gpu.stream)
      .and_then([&] {
        return UploadMoving(ordered.moving, kind, gas, records, gpu);
      });
}

Result<void> PointAt(const InstanceRecords& records, OptixTraversableHandle gas,
                     cudaStream_t stream) {
  const auto patch = [&](CUdeviceptr first, std::size_t stride,
                         std::uint32_t count) -> Result<void> {
    if (count == 0) {
      return {};
    }
    const std::vector<OptixTraversableHandle> handles(count, gas);
    return ext::cuda::Copy2DAsync(
        {
            .to = ext::cuda::PointerOf(first),
            .to_pitch = stride,
            .from = handles.data(),
            .from_pitch = sizeof(OptixTraversableHandle),
            .row_bytes = sizeof(OptixTraversableHandle),
            .rows = count,
            .kind = cudaMemcpyHostToDevice,
        },
        stream, "cannot point the instances at their GAS");
  };
  return patch(records.still.address() +
                   offsetof(OptixInstance, traversableHandle),
               sizeof(OptixInstance), records.still_count)
      .and_then([&] {
        return patch(records.transforms.address(), records.transform_stride,
                     records.moving_count);
      });
}

std::uint32_t CountOf(const InstanceRecords& records) {
  return records.still_count + records.moving_count;
}

Result<void> CopyRecords(const InstanceRecords& records, DeviceBuffer& input,
                         std::uint32_t first, cudaStream_t stream) {
  const std::size_t offset = first * sizeof(OptixInstance);
  return input.CopyFrom(records.still, offset, stream).and_then([&] {
    return input.CopyFrom(records.moving, offset + records.still.size(),
                          stream);
  });
}

}  // namespace umbra::device::gpu::backend
