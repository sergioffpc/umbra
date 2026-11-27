// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// A prototype as a GPU holds it: its arrays, its GAS and the records of its
// instances in the IAS.

#ifndef UMBRA_GPU_BACKEND_PROTOTYPE_H_
#define UMBRA_GPU_BACKEND_PROTOTYPE_H_

#include <driver_types.h>
#include <optix_types.h>

#include <cstdint>
#include <string>
#include <vector>

#include "accel.h"
#include "buffer.h"
#include "held.h"
#include "instances.h"
#include "support.h"
#include "umbra/core/safety.h"
#include "umbra/device/change_set.h"
#include "umbra/device/render_data.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::gpu::backend {

// Whether a prototype's points have ever changed without its topology: its
// GAS is then built with OPTIX_BUILD_FLAG_ALLOW_UPDATE, so that it refits.
enum class Deformation : std::uint8_t {
  kRigid,
  kDeforms,
};

// A prototype in a GPU's memory.
struct GpuPrototype {
  device::PrototypeId id{};
  // For the messages.
  std::string path;
  // Every motion key's points, one after another.
  DeviceBuffer points;
  std::uint32_t point_count = 0;
  std::uint32_t motion_keys = 0;
  DeviceBuffer triangles;
  std::uint32_t triangle_count = 0;
  DeviceBuffer source_faces;
  DeviceBuffer face_vertex_counts;
  device::Sidedness sidedness = device::Sidedness::kSingleSided;
  HeldPrimvar normals;
  std::vector<HeldPrimvar> primvars;
  DeviceBuffer materials;
  DeviceBuffer triangle_materials;
  AccelStructure gas;
  Deformation deformation = Deformation::kRigid;
  // The refits of its GAS since it was last built.
  std::uint32_t refits = 0;
  // The records of its instances in the IAS.
  InstanceRecords instances;
  // Each primvar of its instances, one value per instance, in the order of
  // their records.
  std::vector<HeldPrimvar> instance_primvars;
};

// Uploads `prototype` and builds its GAS and the records of its instances.
[[nodiscard]] Result<GpuPrototype> AddPrototype(
    const device::Prototype& prototype, const Gpu& gpu);

// Applies `update` to `held`: uploads the members its flags name and
// rebuilds or refits the GAS as they ask.
[[nodiscard]] Result<void> UpdatePrototype(
    const device::UpdatedPrototype& update, GpuPrototype& held, const Gpu& gpu);

// The records `held` adds to the IAS: none while it has no triangles.
[[nodiscard]] std::uint32_t InstanceCount(const GpuPrototype& held);

// Copies the records of `held` into `input`, an array of OptixInstance, from
// the record `first` on.
[[nodiscard]] Result<void> CopyInstances(const GpuPrototype& held,
                                         DeviceBuffer& input,
                                         std::uint32_t first,
                                         cudaStream_t stream);

}  // namespace umbra::gpu::backend

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_GPU_BACKEND_PROTOTYPE_H_
