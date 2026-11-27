// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "prototype.h"

#include <driver_types.h>
#include <optix_types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include "accel.h"
#include "held.h"
#include "instances.h"
#include "support.h"
#include "umbra/core/log.h"
#include "umbra/device/change_set.h"
#include "umbra/device/device.h"
#include "umbra/device/render_data.h"
#include "umbra/ext/cuda/owners.h"
#include "umbra/ext/cuda/runtime.h"

namespace umbra::device::gpu::backend {

using ext::cuda::DeviceBuffer;
using ext::cuda::Replace;

namespace {

using ::umbra::device::ChangeFlags;

// Uploads the points of every motion key, one after another.
Result<void> UploadPoints(
    std::span<const std::span<const device::Float3>> points, GpuPrototype& held,
    cudaStream_t stream) {
  const std::size_t count = points.empty() ? 0 : points.front().size();
  const std::size_t bytes = count * sizeof(device::Float3) * points.size();
  if (held.points.size() != bytes) {
    auto buffer = DeviceBuffer::Allocate(bytes);
    if (!buffer.has_value()) {
      return std::unexpected(buffer.error());
    }
    held.points = std::move(*buffer);
  }
  std::size_t offset = 0;
  for (const std::span<const device::Float3> key : points) {
    if (auto written = held.points.Write(std::as_bytes(key), offset, stream);
        !written.has_value()) {
      return written;
    }
    offset += key.size_bytes();
  }
  held.point_count = static_cast<std::uint32_t>(count);
  held.motion_keys = static_cast<std::uint32_t>(points.size());
  return {};
}

// Uploads the triangles of `prototype` and what follows them.
Result<void> UploadTopology(const device::Prototype& prototype,
                            GpuPrototype& held, cudaStream_t stream) {
  held.triangle_count = static_cast<std::uint32_t>(prototype.triangles.size());
  held.sidedness = prototype.sidedness;
  return Replace(held.triangles, prototype.triangles, stream)
      .and_then([&] {
        return Replace(held.source_faces, prototype.source_faces, stream);
      })
      .and_then([&] {
        return Replace(held.face_vertex_counts, prototype.face_vertex_counts,
                       stream);
      });
}

// Uploads the normals and the other primvars of `prototype`.
Result<void> UploadPrimvars(const device::Prototype& prototype,
                            GpuPrototype& held, cudaStream_t stream) {
  auto normals = UploadPrimvar(prototype.normals, stream);
  if (!normals.has_value()) {
    return std::unexpected(normals.error());
  }
  std::vector<HeldPrimvar> primvars;
  primvars.reserve(prototype.primvars.size());
  for (const device::Primvar& primvar : prototype.primvars) {
    auto uploaded = UploadPrimvar(primvar, stream);
    if (!uploaded.has_value()) {
      return std::unexpected(uploaded.error());
    }
    primvars.push_back(std::move(*uploaded));
  }
  held.normals = std::move(*normals);
  held.primvars = std::move(primvars);
  return {};
}

// Uploads the materials of `prototype` and those of its triangles.
Result<void> UploadMaterials(const device::Prototype& prototype,
                             GpuPrototype& held, cudaStream_t stream) {
  return Replace(held.materials, prototype.materials, stream).and_then([&] {
    return Replace(held.triangle_materials, prototype.triangle_materials,
                   stream);
  });
}

// How a GAS is made from its prototype's arrays.
enum class GasBuild : std::uint8_t {
  kBuild,
  kRefit,
};

// Builds the GAS of `held`, or refits it.
Result<void> BuildGas(GpuPrototype& held, GasBuild build, const Gpu& gpu) {
  std::vector<CUdeviceptr> keys;
  keys.reserve(held.motion_keys);
  const std::size_t key_bytes = held.point_count * sizeof(device::Float3);
  for (std::uint32_t key = 0; key < held.motion_keys; ++key) {
    keys.push_back(held.points.address() + (key * key_bytes));
  }
  const unsigned flags = OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT;
  OptixBuildInputTriangleArray triangles{};
  triangles.vertexBuffers = keys.data();
  triangles.numVertices = held.point_count;
  triangles.vertexFormat = OPTIX_VERTEX_FORMAT_FLOAT3;
  triangles.vertexStrideInBytes = sizeof(device::Float3);
  triangles.indexBuffer = held.triangles.address();
  triangles.numIndexTriplets = held.triangle_count;
  triangles.indexFormat = OPTIX_INDICES_FORMAT_UNSIGNED_INT3;
  triangles.indexStrideInBytes = sizeof(device::Triangle);
  triangles.flags = &flags;
  triangles.numSbtRecords = 1;
  const std::array inputs = {
      OptixBuildInput{
          .type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES,
          .triangleArray = triangles,
      },
  };
  const unsigned build_flags =
      held.deformation == Deformation::kDeforms
          ? OPTIX_BUILD_FLAG_PREFER_FAST_TRACE | OPTIX_BUILD_FLAG_ALLOW_UPDATE
          : OPTIX_BUILD_FLAG_PREFER_FAST_TRACE;
  return BuildAccel(
      gpu,
      BuildOptions(build_flags,
                   build == GasBuild::kRefit ? OPTIX_BUILD_OPERATION_UPDATE
                                             : OPTIX_BUILD_OPERATION_BUILD,
                   held.motion_keys),
      inputs, held.gas);
}

// The GAS of a prototype with no triangle: none.
Result<void> BuildGasOrNone(GpuPrototype& held, const Gpu& gpu) {
  if (held.triangle_count == 0) {
    held.gas = {};
    return {};
  }
  return BuildGas(held, GasBuild::kBuild, gpu);
}

// Copies the first `width` numbers of `from` into `into` at `offset`, as many
// as fit.
template <typename T>
void CopyValue(std::span<const T> from, std::vector<T>& into,
               std::size_t offset, std::size_t width) {
  if (offset >= into.size()) {
    return;
  }
  const std::size_t count =
      std::min({width, from.size(), into.size() - offset});
  std::ranges::copy(from.first(count), std::span(into).subspan(offset).begin());
}

// The primvar named as `first` of every instance of `ordered`, one value
// each: zeros for an instance that does not give it.
Result<HeldPrimvar> PackPrimvar(
    const device::Primvar& first,
    std::span<const device::Instance* const> ordered, cudaStream_t stream) {
  const std::size_t width = first.components;
  const std::size_t numbers = width * ordered.size();
  std::vector<float> floats(first.floats.empty() ? 0 : numbers);
  std::vector<std::int32_t> integers(first.floats.empty() ? numbers : 0);
  std::size_t offset = 0;
  for (const device::Instance* const instance : ordered) {
    const auto own = std::ranges::find(instance->primvars, first.name,
                                       &device::Primvar::name);
    if (own != instance->primvars.end() && own->components == width) {
      CopyValue(own->floats, floats, offset, width);
      CopyValue(own->integers, integers, offset, width);
    }
    offset += width;
  }
  const device::Primvar packed{
      .name = first.name,
      .rate = device::PrimvarRate::kConstant,
      .components = first.components,
      .floats = floats,
      .integers = integers,
      .indices = {},
  };
  return UploadPrimvar(packed, stream);
}

// The first primvar of each name among those of `ordered`.
std::vector<const device::Primvar*> FirstOfEachName(
    std::span<const device::Instance* const> ordered) {
  std::vector<const device::Primvar*> firsts;
  const auto primvars_of = [](const device::Instance* instance) {
    return instance->primvars;
  };
  for (const device::Primvar& primvar :
       ordered | std::views::transform(primvars_of) | std::views::join) {
    if (!std::ranges::contains(firsts, primvar.name, &device::Primvar::name)) {
      firsts.push_back(&primvar);
    }
  }
  return firsts;
}

// Uploads the primvars of the instances, in the order of their records.
Result<void> UploadInstancePrimvars(
    std::span<const device::Instance* const> ordered, GpuPrototype& held,
    cudaStream_t stream) {
  std::vector<HeldPrimvar> primvars;
  for (const device::Primvar* const first : FirstOfEachName(ordered)) {
    auto packed = PackPrimvar(*first, ordered, stream);
    if (!packed.has_value()) {
      return std::unexpected(packed.error());
    }
    primvars.push_back(std::move(*packed));
  }
  held.instance_primvars = std::move(primvars);
  return {};
}

// Uploads the records of every instance of `instances`, and their primvars.
Result<void> UploadInstances(std::span<const device::Instance> instances,
                             GpuPrototype& held, const Gpu& gpu) {
  const OrderedInstances ordered = Order(instances);
  std::vector<const device::Instance*> all = ordered.still;
  all.insert(all.end(), ordered.moving.begin(), ordered.moving.end());
  const RecordKind kind{
      .prim = std::to_underlying(held.id),
      .sbt_offset = kTriangleHitGroup,
      .mask = kGeometryMask,
  };
  return UploadRecords(ordered, kind, held.gas.handle, held.instances, gpu)
      .and_then([&] { return UploadInstancePrimvars(all, held, gpu.stream); })
      .and_then([&] {
        return ext::cuda::Synchronize(gpu.stream,
                                      "cannot upload the instances");
      });
}

// Logs that the GAS of `held` was built or rebuilt (`done`), and why.
void LogGas(const GpuPrototype& held, std::string_view done,
            std::string_view why, const Gpu& gpu) {
  Log(core::LogLevel::kDebug,
      "{}: {} the GAS of {}{}: {} triangles, {} motion key{}{}", gpu.name, done,
      held.path, why, held.triangle_count, held.motion_keys,
      held.motion_keys == 1 ? "" : "s",
      held.deformation == Deformation::kDeforms ? ", refittable" : "");
}

// The motion keys and points of a GAS, which a refit keeps.
struct GasShape {
  std::uint32_t motion_keys = 0;
  std::uint32_t point_count = 0;
};

// Why the GAS of `held` must be rebuilt for new points, built from `old`;
// empty when it refits.
std::string_view RebuildReason(const GpuPrototype& held, GasShape old) {
  if (held.deformation == Deformation::kRigid) {
    return " (it deforms)";
  }
  if (old.motion_keys != held.motion_keys ||
      old.point_count != held.point_count) {
    return " (its motion keys or points changed)";
  }
  if (held.refits >= static_cast<std::uint32_t>(device::kMaxRefits)) {
    return " (after its refits)";
  }
  return {};
}

// Refits the GAS of `held` to its new points, or rebuilds it: the first
// time its points change, so that it refits from then on, when their number
// of motion keys or of points changed, and after kMaxRefits refits in a
// row.
Result<void> RefitOrRebuild(GpuPrototype& held, GasShape old, const Gpu& gpu) {
  if (held.gas.handle == 0) {
    return {};
  }
  const std::string_view why = RebuildReason(held, old);
  if (!why.empty()) {
    held.deformation = Deformation::kDeforms;
    held.refits = 0;
    return BuildGas(held, GasBuild::kBuild, gpu).transform([&] {
      LogGas(held, "rebuilt", why, gpu);
    });
  }
  ++held.refits;
  return BuildGas(held, GasBuild::kRefit, gpu).transform([&] {
    Log(core::LogLevel::kDebug, "{}: refitted the GAS of {} ({} of {})",
        gpu.name, held.path, held.refits, device::kMaxRefits);
  });
}

// Applies the geometry of `update` to `held`: rebuilds its GAS for new
// triangles, refits it for new points.
Result<void> UpdateGeometry(const device::UpdatedPrototype& update,
                            GpuPrototype& held, const Gpu& gpu) {
  const GasShape old{
      .motion_keys = held.motion_keys,
      .point_count = held.point_count,
  };
  if (HasAny(update.changes, ChangeFlags::kTopology)) {
    held.refits = 0;
    return UploadPoints(update.prototype.points, held, gpu.stream)
        .and_then(
            [&] { return UploadTopology(update.prototype, held, gpu.stream); })
        .and_then([&] { return BuildGasOrNone(held, gpu); })
        .transform([&] {
          LogGas(held, held.gas.handle == 0 ? "dropped" : "rebuilt",
                 " (new topology)", gpu);
        });
  }
  if (HasAny(update.changes, ChangeFlags::kPoints)) {
    return UploadPoints(update.prototype.points, held, gpu.stream)
        .and_then([&] { return RefitOrRebuild(held, old, gpu); });
  }
  return {};
}

// Applies what `update` gives besides geometry to `held`.
Result<void> UpdateShading(const device::UpdatedPrototype& update,
                           GpuPrototype& held, cudaStream_t stream) {
  if (HasAny(update.changes, ChangeFlags::kPrimvars)) {
    if (auto uploaded = UploadPrimvars(update.prototype, held, stream);
        !uploaded.has_value()) {
      return uploaded;
    }
  }
  if (HasAny(update.changes, ChangeFlags::kMaterials)) {
    return UploadMaterials(update.prototype, held, stream);
  }
  return {};
}

}  // namespace

Result<GpuPrototype> AddPrototype(const device::Prototype& prototype,
                                  const Gpu& gpu) {
  GpuPrototype held;
  held.id = prototype.id;
  held.path = prototype.path;
  return UploadPoints(prototype.points, held, gpu.stream)
      .and_then([&] { return UploadTopology(prototype, held, gpu.stream); })
      .and_then([&] { return UploadPrimvars(prototype, held, gpu.stream); })
      .and_then([&] { return UploadMaterials(prototype, held, gpu.stream); })
      .and_then([&] { return BuildGasOrNone(held, gpu); })
      .and_then([&] {
        LogGas(held, "built", "", gpu);
        return UploadInstances(prototype.instances, held, gpu);
      })
      .transform([&] { return std::move(held); });
}

Result<void> UpdatePrototype(const device::UpdatedPrototype& update,
                             GpuPrototype& held, const Gpu& gpu) {
  const OptixTraversableHandle old_gas = held.gas.handle;
  return UpdateGeometry(update, held, gpu)
      .and_then([&] { return UpdateShading(update, held, gpu.stream); })
      .and_then([&] -> Result<void> {
        if (HasAny(update.changes, ChangeFlags::kPlacement)) {
          return UploadInstances(update.prototype.instances, held, gpu);
        }
        if (held.gas.handle != old_gas) {
          return PointAt(held.instances, held.gas.handle, gpu.stream);
        }
        return {};
      })
      .and_then([&] {
        return ext::cuda::Synchronize(gpu.stream, "cannot update a prototype");
      });
}

std::uint32_t InstanceCount(const GpuPrototype& held) {
  return held.gas.handle == 0 ? 0 : CountOf(held.instances);
}

Result<void> CopyInstances(const GpuPrototype& held, DeviceBuffer& input,
                           std::uint32_t first, cudaStream_t stream) {
  if (InstanceCount(held) == 0) {
    return {};
  }
  return CopyRecords(held.instances, input, first, stream);
}

}  // namespace umbra::device::gpu::backend
