// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// Owners of OptiX's objects: a context, and the modules, program groups and
// pipelines compiled on it.

#ifndef UMBRA_EXT_OPTIX_OWNERS_H_
#define UMBRA_EXT_OPTIX_OWNERS_H_

#include <optix_types.h>

#include <format>

#include "umbra/core/handle_owner.h"
#include "umbra/core/lazy_format.h"
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
  // the current GPU. `what` is formatted only when it fails.
  [[nodiscard]] static Result<DeviceContext> Create(
      const core::LazyFormat& what);

  template <typename... Args>
  [[nodiscard]] static Result<DeviceContext> Create(
      std::format_string<Args...> what, const Args&... args) {
    return Create(core::LazyFormat(what.get(), std::make_format_args(args...)));
  }

  // Destroys the context now, if it holds one, and fails as OptiX does.
  // `what` is formatted only when it fails.
  [[nodiscard]] Result<void> Destroy(const core::LazyFormat& what);

  template <typename... Args>
  [[nodiscard]] Result<void> Destroy(std::format_string<Args...> what,
                                     const Args&... args) {
    return Destroy(
        core::LazyFormat(what.get(), std::make_format_args(args...)));
  }

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
