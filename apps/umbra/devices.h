// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The devices umbra renders on (docs/adr/0004-render-devices.md): one per
// NUMA node with CPUs (cpu0, cpu1, …) and one per usable GPU, by CUDA ordinal
// (gpu0, gpu1, …). umbra reads the hardware behind them, a render picks them
// with --devices, and they are created here; `umbra devices` lists them
// (see device_listing.h).

#ifndef UMBRA_APPS_UMBRA_DEVICES_H_
#define UMBRA_APPS_UMBRA_DEVICES_H_

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "umbra/core/cpu_topology.h"
#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/delegate/session.h"
#include "umbra/device/device.h"
#include "umbra/device/gpu/module.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::devices {

// The hardware of the machine, as umbra sees it.
struct Hardware {
  std::vector<core::CpuNode> cpus;
  // The GPUs, or why there are none (no module, no driver).
  std::expected<device::gpu::GpuDevices, core::Error> gpus;
  // The CPU's widest vector ISA ("AVX2").
  std::string_view cpu_isa;
  // The ISA Embree chose ("AVX2").
  std::string_view embree_isa;
  // The widest batch OSL shades, or 0.
  int osl_batch_width = 0;
};

// Reads the hardware: the CPU topology from sysfs, the GPUs from the GPU
// module, and what Embree and OSL chose. The libraries must be initialized.
// Fails when no CPU is described.
[[nodiscard]] std::expected<Hardware, core::Error> ReadHardware();

// What a render device is.
enum class DeviceKind : std::uint8_t {
  kCpu,
  kGpu,
};

// The name of a render device: "cpu0" for NUMA node 0, "gpu0" for CUDA
// ordinal 0.
[[nodiscard]] std::string DeviceName(DeviceKind kind, int index);

// A device a render can use.
struct RenderDevice {
  DeviceKind kind = DeviceKind::kCpu;
  // The NUMA node for a CPU, the CUDA ordinal for a GPU.
  int index = 0;
  // Its DeviceName.
  std::string name;
};

// The devices of `hardware` a render can use: the CPU nodes, then the usable
// GPUs.
[[nodiscard]] std::vector<RenderDevice> RenderDevices(const Hardware& hardware);

// The devices a --devices value names ("cpu0,gpu0"), in its order, among
// `devices`; every device when `spec` is empty. Fails with kInvalidArgument on
// an empty, unknown or repeated name.
[[nodiscard]] std::expected<std::vector<RenderDevice>, core::Error>
ResolveDevices(std::string_view spec, std::span<const RenderDevice> devices);

// The devices of a render, made from the RenderDevices it chose, and the GPU
// module the GPU devices are made through, which outlives them.
struct CreatedDevices {
  std::optional<device::gpu::Module> module;
  std::vector<std::unique_ptr<device::Device>> owned;
  // The devices of `owned`, in its order, which the render commits to.
  std::vector<device::Device*> devices;
  // The GPUs among them, which the denoiser chooses from.
  std::vector<delegate::Gpu> gpus;
};

// The devices that `chosen` names, in its order, which a render commits to:
// a CPU device for each CPU node, and a GPU device for each GPU, made
// through the GPU module, which is loaded only when `chosen` has a GPU. When
// the device component logs at debug level, a diagnostic device follows
// them, which checks what every commit gives. Fails with the cause, naming
// the device, when the module cannot be loaded or a GPU cannot be used.
[[nodiscard]] std::expected<CreatedDevices, core::Error> CreateDevices(
    std::span<const RenderDevice> chosen);

}  // namespace umbra::devices

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_APPS_UMBRA_DEVICES_H_
