// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// Owners of OptiX's objects: a context, and the modules, program groups and
// pipelines compiled on it.

#ifndef UMBRA_EXT_OPTIX_OWNERS_H_
#define UMBRA_EXT_OPTIX_OWNERS_H_

#include <optix_types.h>

#include <string_view>

#include "umbra/core/handle_owner.h"
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

  // A context on the current CUDA context, which the CUDA runtime made for
  // the current GPU.
  [[nodiscard]] static Result<DeviceContext> Create(std::string_view what);

  // Destroys the context now, if it holds one, and fails as OptiX does.
  [[nodiscard]] Result<void> Destroy(std::string_view what);

  // The context, which it must hold.
  [[nodiscard]] OptixDeviceContext get() const { return context_.get(); }

 private:
  // Destroys `context`, and logs a failure.
  static void DestroyLogged(OptixDeviceContext context);

  explicit DeviceContext(OptixDeviceContext context) : context_(context) {}

  core::HandleOwner<OptixDeviceContext, &DeviceContext::DestroyLogged> context_;
};

// The destroyers of the objects compiled on a context, which ignore a
// failure: nothing is left to do with an object OptiX cannot destroy.
void DestroyModule(OptixModule module);
void DestroyProgramGroup(OptixProgramGroup group);
void DestroyPipeline(OptixPipeline pipeline);

// A module, a program group and a pipeline, destroyed with their owner.
using Module = core::HandleOwner<OptixModule, DestroyModule>;
using ProgramGroup = core::HandleOwner<OptixProgramGroup, DestroyProgramGroup>;
using Pipeline = core::HandleOwner<OptixPipeline, DestroyPipeline>;

}  // namespace umbra::ext::optix

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_OPTIX_OWNERS_H_
