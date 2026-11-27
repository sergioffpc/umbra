// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// Owners of OptiX's objects: a context, and the modules, program groups and
// pipelines compiled on it.

#ifndef UMBRA_EXT_OPTIX_OWNERS_H_
#define UMBRA_EXT_OPTIX_OWNERS_H_

#include <optix_types.h>

#include <memory>
#include <string_view>
#include <utility>

#include "umbra/core/safety.h"
#include "umbra/ext/optix/errors.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::optix {

// An OptiX context on the current CUDA context, destroyed with it, or by
// Destroy. It logs OptiX's warnings and errors as warnings. A destroy that
// fails in the destructor is logged as a warning.
class UMBRA_OWNER DeviceContext {
 public:
  DeviceContext() = default;
  DeviceContext(const DeviceContext&) = delete;
  DeviceContext& operator=(const DeviceContext&) = delete;
  DeviceContext(DeviceContext&& other) noexcept
      : context_(std::exchange(other.context_, nullptr)) {}
  DeviceContext& operator=(DeviceContext&& other) noexcept {
    std::swap(context_, other.context_);
    return *this;
  }
  ~DeviceContext();

  // A context on the current CUDA context, which the CUDA runtime made for
  // the current GPU.
  [[nodiscard]] static Result<DeviceContext> Create(std::string_view what);

  // Destroys the context now, if it holds one, and fails as OptiX does.
  [[nodiscard]] Result<void> Destroy(std::string_view what);

  // The context, which it must hold.
  [[nodiscard]] OptixDeviceContext get() const {
    return static_cast<OptixDeviceContext>(context_);
  }

 private:
  explicit DeviceContext(OptixDeviceContext UMBRA_NULLABLE context)
      : context_(context) {}

  OptixDeviceContext UMBRA_NULLABLE context_ = nullptr;
};

// The destroyers of the objects compiled on a context, which ignore a
// failure: nothing is left to do with an object OptiX cannot destroy.
struct ModuleDestroyer {
  void operator()(OptixModule module) const;
};
struct ProgramGroupDestroyer {
  void operator()(OptixProgramGroup group) const;
};
struct PipelineDestroyer {
  void operator()(OptixPipeline pipeline) const;
};

// A module, a program group and a pipeline, destroyed with their owner.
using Module = std::unique_ptr<OptixModule_t, ModuleDestroyer>;
using ProgramGroup =
    std::unique_ptr<OptixProgramGroup_t, ProgramGroupDestroyer>;
using Pipeline = std::unique_ptr<OptixPipeline_t, PipelineDestroyer>;

}  // namespace umbra::ext::optix

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_OPTIX_OWNERS_H_
