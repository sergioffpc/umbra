// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// A device on a GPU, as the GPU module holds it: its stream, its OptiX
// context, and what it holds of the prims committed to it, in its own
// memory (docs/adr/0013-devices-render-from-their-own-memory.md).

#ifndef UMBRA_GPU_BACKEND_DEVICE_H_
#define UMBRA_GPU_BACKEND_DEVICE_H_

#include <driver_types.h>
#include <optix_types.h>

#include <memory>
#include <string>

#include "accel.h"
#include "buffer.h"
#include "held.h"
#include "light.h"
#include "prototype.h"
#include "support.h"
#include "umbra/core/safety.h"
#include "umbra/device/change_set.h"
#include "umbra/device/render_data.h"
#include "umbra/gpu/module_abi.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::gpu {

// A device on one GPU. Every call makes its GPU current on the calling
// thread, so that each device may be called from a thread of its own.
class ModuleDevice {
 public:
  // A device on the GPU of CUDA ordinal `ordinal`. Fails when the GPU is
  // older than sm_120 or CUDA or OptiX cannot be used on it.
  [[nodiscard]] static backend::Result<std::unique_ptr<ModuleDevice>> Create(
      int ordinal);

  // Takes `stream` and `context`, which it destroys.
  ModuleDevice(int ordinal, cudaStream_t stream, OptixDeviceContext context);
  ModuleDevice(const ModuleDevice&) = delete;
  ModuleDevice& operator=(const ModuleDevice&) = delete;
  ModuleDevice(ModuleDevice&&) = delete;
  ModuleDevice& operator=(ModuleDevice&&) = delete;
  ~ModuleDevice();

  // Applies `changes`: its removals, then its updates and its additions.
  // Uploads prototypes, lights and cameras, builds or refits the GAS of each
  // prototype as its change flags ask, rebuilds the GAS of lights when a
  // light changed and the IAS at every commit, and waits for all of it. Of
  // materials and textures, it holds the ids that prototypes and lights
  // refer to.
  [[nodiscard]] backend::Result<void> Commit(const device::ChangeSet& changes);

 private:
  [[nodiscard]] backend::Gpu gpu() const UMBRA_LIFETIMEBOUND;

  [[nodiscard]] backend::Result<void> ApplyPrototypes(
      const device::PrototypeChanges& changes);
  [[nodiscard]] backend::Result<void> BuildLightGas();
  [[nodiscard]] backend::Result<void> BuildIas();

  int ordinal_;
  std::string name_;
  cudaStream_t stream_;
  OptixDeviceContext context_;
  backend::IdTable<backend::GpuPrototype, device::PrototypeId> prototypes_{
      "prototype"};
  backend::IdTable<backend::GpuLight, device::LightId> lights_{"light"};
  backend::IdTable<backend::GpuCamera, device::CameraId> cameras_{"camera"};
  backend::AccelStructure light_gas_;
  // The LightPrimitive of each primitive of light_gas_.
  backend::DeviceBuffer light_primitives_;
  backend::AccelStructure ias_;
};

}  // namespace umbra::gpu

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_GPU_BACKEND_DEVICE_H_
