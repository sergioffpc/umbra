# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Initial cache for OpenQMC (cmake -C): the sampler headers and the
# OpenQMC::OpenQMC package only, without tools or tests. The common settings
# of the layer come from layers/deps.cmake.
#
# The headers use no exceptions or RTTI.

set(OPENQMC_BUILD_TOOLS OFF CACHE BOOL "")
set(OPENQMC_BUILD_TESTING OFF CACHE BOOL "")

# Header-only: the blue noise tables stay constexpr arrays in the headers
# instead of moving into a compiled library. The GPU module needs them in
# device code, which a host library cannot provide.
set(OPENQMC_ENABLE_BINARY OFF CACHE BOOL "")

# The SIMD path follows the compiler's flags: arch.h picks the AVX2 code under
# the umbra targets' -march=x86-64-v3. Scalar and GPU would instead add
# OQMC_FORCE_SCALAR to the target's interface, for every consumer.
set(OPENQMC_ARCH_TYPE AVX CACHE STRING "")
