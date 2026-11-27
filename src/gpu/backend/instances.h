// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The records in the IAS of the instances of a prim that has a GAS of its
// own: a prototype, or a light with a shape.

#ifndef UMBRA_GPU_BACKEND_INSTANCES_H_
#define UMBRA_GPU_BACKEND_INSTANCES_H_

#include <driver_types.h>
#include <optix_types.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "accel.h"
#include "buffer.h"
#include "support.h"
#include "umbra/core/safety.h"
#include "umbra/device/render_data.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::gpu::backend {

// What every record of a prim's instances tells the IAS: the id of the
// prim, its hit group and its visibility mask.
struct RecordKind {
  std::uint32_t prim = 0;
  std::uint32_t sbt_offset = kTriangleHitGroup;
  std::uint32_t mask = kGeometryMask;
};

// The records of a prim's instances in the IAS, still ones first: an
// OptixInstance each, which points to the GAS for a still instance, and to
// its OptixMatrixMotionTransform for a moving one.
struct InstanceRecords {
  DeviceBuffer still;
  std::uint32_t still_count = 0;
  DeviceBuffer moving;
  std::uint32_t moving_count = 0;
  // The motion transforms of the moving instances, `transform_stride`
  // bytes apart.
  DeviceBuffer transforms;
  std::size_t transform_stride = 0;
};

// The instances of a prim, still ones first.
struct OrderedInstances {
  std::vector<const device::Instance*> still;
  std::vector<const device::Instance*> moving;
};

// `instances`, still ones first: an instance moves when it has more than
// one key.
[[nodiscard]] OrderedInstances Order(
    std::span<const device::Instance> instances);

// Uploads the records of `ordered`, each of the `kind` given, pointing to
// `gas`, into `records`.
[[nodiscard]] Result<void> UploadRecords(const OrderedInstances& ordered,
                                         RecordKind kind,
                                         OptixTraversableHandle gas,
                                         InstanceRecords& records,
                                         const Gpu& gpu);

// Points every record of `records` at `gas`: each still instance, and the
// motion transform of each moving one.
[[nodiscard]] Result<void> PointAt(const InstanceRecords& records,
                                   OptixTraversableHandle gas,
                                   cudaStream_t stream);

// The number of records of `records`.
[[nodiscard]] std::uint32_t CountOf(const InstanceRecords& records);

// Copies `records` into `input`, an array of OptixInstance, from the record
// `first` on.
[[nodiscard]] Result<void> CopyRecords(const InstanceRecords& records,
                                       DeviceBuffer& input, std::uint32_t first,
                                       cudaStream_t stream);

}  // namespace umbra::gpu::backend

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_GPU_BACKEND_INSTANCES_H_
