// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "support.h"

#include "umbra/core/log_hook.h"

namespace umbra::device::gpu::backend {

core::LogHook& Logging() {
  static core::LogHook hook;
  return hook;
}

}  // namespace umbra::device::gpu::backend
