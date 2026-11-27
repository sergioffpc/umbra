# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Compiler, language and code generation settings shared by every umbra
# target:
#
#   - C++23 without compiler extensions, built with clang against libc++,
#     compiler-rt and libunwind, linked with lld: no GCC runtime library;
#   - the LLVM binary tools (llvm-ar, llvm-ranlib, llvm-nm), required to
#     archive the LTO bitcode of the static modules;
#   - no exceptions and no RTTI (the bridges and the render delegate
#     re-enable both, through umbra_enable_exceptions);
#   - every build type targets UMBRA_ARCH and uses the same floating-point
#     flags, so Debug and Release compute identical results;
#   - Release adds -O3 (CMake's default), full LTO, whole-program vtables,
#     hidden visibility, no semantic interposition and no PLT;
#   - hidden visibility tells LTO that a class is whole inside umbra, so
#     whole-program vtables devirtualize its virtual calls, and gives its
#     type_info a copy of umbra's own. Both are wrong for the classes of a
#     third-party library, whose shared objects hold subclasses umbra never
#     sees and the type_info that libc++ compares by address. The bridges
#     and the render delegate, the only code that includes third-party
#     headers, keep the default visibility (umbra_enable_exceptions), as
#     Debug does; the rest of umbra keeps both for its own classes;
#   - Release keeps the frame pointers, including in leaf functions, so that
#     profilers (perf) unwind call stacks cheaply and reliably; the cost is
#     about 1%, and the major Linux distributions build this way too;
#   - source paths in __FILE__ and std::source_location are relative to the
#     repository ("src/gpu/module.cc"), as trace lines show them;
#   - UMBRA_TIME_TRACE writes a -ftime-trace JSON file per translation unit
#     next to its object file.
#
# The settings apply through umbra_apply_compiler_settings(<target>), called
# by the helpers in UmbraModules.cmake.

include_guard(GLOBAL)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR
          "umbra builds with clang only (found ${CMAKE_CXX_COMPILER_ID}); "
          "build inside the devcontainer")
endif()

set(UMBRA_ARCH "x86-64-v3" CACHE STRING
    "Target architecture (-march) of every build type")
option(UMBRA_TIME_TRACE "Write a -ftime-trace file per translation unit" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# umbra uses no C++20 modules; skip the dependency scan they need.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The LLVM binary tools that sit next to the compiler.
cmake_path(GET CMAKE_CXX_COMPILER PARENT_PATH umbra_compiler_dir)
file(REAL_PATH "${CMAKE_CXX_COMPILER}" umbra_compiler_real)
cmake_path(GET umbra_compiler_real PARENT_PATH umbra_compiler_real_dir)
foreach(umbra_tool IN ITEMS ar ranlib nm)
  string(TOUPPER "${umbra_tool}" umbra_tool_var)
  find_program(UMBRA_LLVM_${umbra_tool_var} NAMES llvm-${umbra_tool}
               HINTS "${umbra_compiler_dir}" "${umbra_compiler_real_dir}"
               REQUIRED)
  set(CMAKE_${umbra_tool_var} "${UMBRA_LLVM_${umbra_tool_var}}"
      CACHE FILEPATH "" FORCE)
endforeach()

function(umbra_apply_compiler_settings target)
  target_compile_options(${target} PRIVATE
    -stdlib=libc++
    -fno-exceptions
    -fno-rtti
    -march=${UMBRA_ARCH}
    -fno-math-errno
    -ffp-contract=on
    -fno-trapping-math
    -fmacro-prefix-map=${PROJECT_SOURCE_DIR}/=
    $<$<BOOL:${UMBRA_TIME_TRACE}>:-ftime-trace>
    $<$<CONFIG:Release>:-flto=full -fwhole-program-vtables
      -fvisibility=hidden -fvisibility-inlines-hidden
      -fno-semantic-interposition -fno-plt
      -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer>)
  target_link_options(${target} PRIVATE
    -stdlib=libc++
    -rtlib=compiler-rt
    -unwindlib=libunwind
    -fuse-ld=lld
    $<$<CONFIG:Release>:-flto=full -fwhole-program-vtables>)
endfunction()
