# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Compiler-enforced safety rules of every umbra target: the warnings that
# back the macros of include/umbra/core/safety.h (unsafe buffer usage,
# lifetimes, thread safety, nullability), the implicit conversion warnings and
# the uninitialized warnings, all of them errors (UmbraWarnings.cmake turns
# every warning into an error).
#
#   - Buffers: pointer arithmetic and raw indexing are errors outside the
#     UMBRA_UNSAFE_BUFFER_BEGIN/END regions.
#   - Lifetimes: dangling references, pointers and views, including those the
#     lifetimebound and lifetime_capture_by annotations reveal.
#   - Thread safety: Clang Thread Safety Analysis, with the annotations of
#     libc++'s own mutexes and locks turned on.
#   - Nullability: every pointer in a header that annotates any pointer must
#     be annotated (UMBRA_ASSUME_NONNULL_BEGIN/END annotate them all), and a
#     nullable pointer never converts implicitly to a non-null one.
#   - Conversions: every implicit numeric conversion that may change a value
#     (narrowing, sign, 64-to-32, integer to float, float to integer) and
#     every implicit float-to-double promotion.
#   - Initialization: reads of possibly uninitialized variables.
#   - Hot path: a function declared [[clang::nonallocating]] or
#     [[clang::nonblocking]] that may allocate, or block too
#     (docs/adr/0003-no-allocations-on-the-hot-path.md).
#
# UMBRA_EXPERIMENTAL_SAFETY (ON by default) adds the analyses clang still
# labels experimental or beta: the lifetime safety analysis and the beta
# thread safety checks. Each flag is probed first and added only when the
# compiler accepts it, so a compiler without it builds and a compiler release
# that makes them noisy can be switched off with one option.
#
# The settings apply through umbra_apply_safety(<target>), called by the
# helpers in UmbraModules.cmake.

include_guard(GLOBAL)

include(CheckCXXCompilerFlag)

option(UMBRA_EXPERIMENTAL_SAFETY
       "Add clang's experimental and beta safety analyses" ON)

set(UMBRA_SAFETY_FLAGS
  # Buffers.
  -Wunsafe-buffer-usage
  # Lifetimes.
  -Wdangling
  -Wdangling-gsl
  -Wdangling-capture
  -Wreturn-stack-address
  # Thread safety.
  -Wthread-safety
  # Nullability.
  -Wnullability
  -Wnullability-completeness
  -Wnullable-to-nonnull-conversion
  # Conversions.
  -Wconversion
  -Wimplicit-int-conversion
  -Wimplicit-float-conversion
  -Wimplicit-int-float-conversion
  -Wfloat-conversion
  -Wshorten-64-to-32
  -Wsign-conversion
  -Wdouble-promotion
  # Initialization.
  -Wuninitialized
  -Wconditional-uninitialized
  -Wsometimes-uninitialized
  -Wuninitialized-const-reference
  # Hot path.
  -Wfunction-effects)

set(UMBRA_EXPERIMENTAL_SAFETY_FLAGS)
if(UMBRA_EXPERIMENTAL_SAFETY)
  foreach(umbra_flag IN ITEMS -Wlifetime-safety -Wthread-safety-beta)
    string(MAKE_C_IDENTIFIER "UMBRA_HAS${umbra_flag}" umbra_flag_var)
    check_cxx_compiler_flag("${umbra_flag}" ${umbra_flag_var})
    if(${umbra_flag_var})
      list(APPEND UMBRA_EXPERIMENTAL_SAFETY_FLAGS "${umbra_flag}")
    endif()
  endforeach()
endif()

function(umbra_apply_safety target)
  target_compile_options(${target} PRIVATE
    ${UMBRA_SAFETY_FLAGS} ${UMBRA_EXPERIMENTAL_SAFETY_FLAGS})
  target_compile_definitions(${target} PRIVATE
    _LIBCPP_ENABLE_THREAD_SAFETY_ANNOTATIONS)
endfunction()
