// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/core/floating_point.h"

#include <pmmintrin.h>
#include <xmmintrin.h>

namespace umbra::core {

void ConfigureThreadFloatingPoint() {
  _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
  _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
}

}  // namespace umbra::core
