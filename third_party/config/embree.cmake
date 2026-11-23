# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Initial cache for Embree (cmake -C): the CPU ray tracing library only, as a
# shared library on oneTBB, without tutorials, ISPC or SYCL. The common
# settings of the layer come from layers/deps.cmake.
#
# Embree compiles itself with -fexceptions and RTTI whatever the flags; its
# API is C, so no exception crosses into umbra.

set(EMBREE_STATIC_LIB OFF CACHE BOOL "")
set(EMBREE_TUTORIALS OFF CACHE BOOL "")
set(EMBREE_ISPC_SUPPORT OFF CACHE BOOL "")
set(EMBREE_SYCL_SUPPORT OFF CACHE BOOL "")
set(EMBREE_STAT_COUNTERS OFF CACHE BOOL "")
set(EMBREE_INSTALL_DEPENDENCIES OFF CACHE BOOL "")

# The standard Linux install layout: lib/libembree4.so.<version>,
# include/embree4/ and lib/cmake/embree-<version>/. Zip mode would install
# embree-vars.sh into the prefix root. patches/embree keeps the layer's
# RUNPATH $ORIGIN, which Embree would otherwise replace with <prefix>/lib; it
# finds libtbb.so.12 next to it.
set(EMBREE_ZIP_MODE OFF CACHE BOOL "")

# oneTBB from the layer, so that Embree's BVH builds share the thread pool of
# everything else in the process instead of oversubscribing it with their own.
set(EMBREE_TASKING_SYSTEM TBB CACHE STRING "")
set(EMBREE_TBB_COMPONENT tbb CACHE STRING "")

# x86-64-v3 is the baseline, so AVX2 is the lowest ISA and the one every
# shared code path is compiled for. An AVX-512 kernel set is selected at run
# time on CPUs that have it. SSE and AVX kernels would never run; APX has no
# hardware yet.
set(EMBREE_MAX_ISA NONE CACHE STRING "")
set(EMBREE_ISA_SSE2 OFF CACHE BOOL "")
set(EMBREE_ISA_SSE42 OFF CACHE BOOL "")
set(EMBREE_ISA_AVX OFF CACHE BOOL "")
set(EMBREE_ISA_AVX2 ON CACHE BOOL "")
set(EMBREE_ISA_AVX512 ON CACHE BOOL "")
set(EMBREE_ISA_APX OFF CACHE BOOL "")

# Ray features of a path tracer: masks and filter functions (alpha cutout,
# shadow transparency), both faces of every primitive, and invalid rays
# reported rather than silently dropped.
set(EMBREE_RAY_MASK ON CACHE BOOL "")
set(EMBREE_FILTER_FUNCTION ON CACHE BOOL "")
set(EMBREE_BACKFACE_CULLING OFF CACHE BOOL "")
set(EMBREE_BACKFACE_CULLING_CURVES OFF CACHE BOOL "")
set(EMBREE_BACKFACE_CULLING_SPHERES OFF CACHE BOOL "")
set(EMBREE_IGNORE_INVALID_RAYS OFF CACHE BOOL "")
# Packets (rtcIntersect4/8/16) are how the wavefront integrator traces a
# queue with SIMD: Embree 4 has no stream API. The packet kernels are hybrid,
# falling back to single-ray traversal when few rays of a packet stay active.
set(EMBREE_RAY_PACKETS ON CACHE BOOL "")
set(EMBREE_COMPACT_POLYS OFF CACHE BOOL "")

# The geometry types of the OptiX backend, so that both backends trace the
# same scene: triangles, curves (hair), points (spheres only; Embree's discs
# have no OptiX counterpart and are not used), user geometry (OptiX custom
# primitives) and instances. No quads, subdivision surfaces or grids: quads
# arrive as triangle pairs, and subdivision (USD with OpenSubdiv) and
# displacement are applied before the scene reaches either backend.
#
# One instance level, as the OptiX backend's instance acceleration structure
# over geometry acceleration structures, which RTX hardware traverses
# natively: nested instances are flattened for both backends.
set(EMBREE_GEOMETRY_TRIANGLE ON CACHE BOOL "")
set(EMBREE_GEOMETRY_CURVE ON CACHE BOOL "")
set(EMBREE_GEOMETRY_POINT ON CACHE BOOL "")
set(EMBREE_GEOMETRY_USER ON CACHE BOOL "")
set(EMBREE_GEOMETRY_INSTANCE ON CACHE BOOL "")
set(EMBREE_GEOMETRY_INSTANCE_ARRAY ON CACHE BOOL "")
set(EMBREE_GEOMETRY_QUAD OFF CACHE BOOL "")
set(EMBREE_GEOMETRY_SUBDIVISION OFF CACHE BOOL "")
set(EMBREE_GEOMETRY_GRID OFF CACHE BOOL "")
set(EMBREE_MAX_INSTANCE_LEVEL_COUNT 1 CACHE STRING "")
