# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Initial cache for oneTBB (cmake -C): libtbb and libtbbbind_2_5 (NUMA and
# hybrid-core aware task arenas, on the system hwloc), without the scalable
# allocator, tests, examples, Python bindings or LTO. The common settings of
# the layer and the hwloc paths come from layers/deps.cmake.
#
# libtbb is built with exceptions and RTTI whatever the flags (it propagates
# exceptions out of tasks); code built with -fno-exceptions uses its headers
# with TBB_USE_EXCEPTIONS detected as 0.

set(TBB_TEST OFF CACHE BOOL "")
set(TBB_EXAMPLES OFF CACHE BOOL "")
set(TBB4PY_BUILD OFF CACHE BOOL "")
set(TBB_STRICT OFF CACHE BOOL "")
set(TBB_ENABLE_IPO OFF CACHE BOOL "")
set(TBB_INSTALL ON CACHE BOOL "")

# No scalable allocator: libtbb loads libtbbmalloc whenever it finds it and
# then allocates its containers' memory there, out of sight of the address
# and thread sanitizers, which only intercept the C library's allocator.
# Without it, libtbb falls back to malloc.
set(TBBMALLOC_BUILD OFF CACHE BOOL "")

# libtbbbind is built against the hwloc that layers/deps.cmake finds and
# passes in CMAKE_HWLOC_2_5_*: oneTBB's own pkg-config search is disabled so
# that it cannot pick up another one. libtbb loads libtbbbind at run time,
# from its own directory, when the machine has several NUMA nodes or core
# types. The Thread Composability Manager is not built: it coordinates
# several oneTBB and OpenMP runtimes in one process, and umbra has one.
set(TBB_DISABLE_HWLOC_AUTOMATIC_SEARCH ON CACHE BOOL "")
set(TCM_BUILD OFF CACHE BOOL "")

# The version scripts of oneTBB's libraries name symbols that a Linux
# build does not define, which lld rejects by default. The layer's linker
# flags are set before this file is loaded, so they are extended here.
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--undefined-version"
    CACHE STRING "" FORCE)
