# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Helpers that define every umbra target with the project settings (compiler,
# warnings, safety, sanitizers, hardening and coverage). Every module and
# loadable module links oneTBB (TBB::tbb), the thread pool of the process, and
# may include its headers directly (docs/adr/0001-onetbb-thread-pool.md);
# executables get it through their modules.
#
#   umbra_add_module(<module> [EXCEPTIONS] SOURCES <file>...
#                    [DEPENDS <module>...] [LINK <target>...])
#
# Adds the static library umbra_<module> (alias umbra::<module>) from the
# sources in src/<module>/. Its public headers are include/umbra/<module>/,
# included as "umbra/<module>/<header>.h". A module sees only its own headers
# and those of the modules in DEPENDS; LINK names other (third-party) targets.
# A module nested in another is named by its path, as device/cpu: its target
# is umbra_device_cpu and its alias umbra::device::cpu, and the modules that
# see its parent's headers see its own too, since they lie under them.
# EXCEPTIONS builds it as a bridge is built, with exceptions and RTTI and
# default visibility in Release: only the render delegate, which derives from
# Hydra's classes (docs/adr/0017-ext-is-a-thin-layer-over-each-dependency.md).
#
#   umbra_add_ext_module(<dependency> SOURCES <file>... [DEPENDS <module>...]
#                        [LINK <target>...])
#
# Adds the bridge module umbra_ext_<dependency> (alias umbra::ext::<dependency>)
# from src/ext/<dependency>/, with headers in include/umbra/ext/<dependency>/.
# It is built with exceptions and RTTI, and with default visibility in
# Release; see src/ext/README.md.
#
#   umbra_add_loadable_module(<target> SOURCES <file>... [HEADERS <module>...]
#                             [LINK <target>...])
#
# Adds a shared library that umbra loads at run time with dlopen (a CMake
# MODULE library, lib<target>.so unless OUTPUT_NAME is set). It sees the public
# headers of the modules in HEADERS but links none of them, since they are not
# position-independent; LINK names other (third-party) targets. Only symbols
# marked [[gnu::visibility("default")]] are exported, and the symbols of the
# static libraries it links stay hidden.
#
#   umbra_add_executable(<name> SOURCES <file>... [DEPENDS <module>...])
#
# Adds an executable linked against the given modules.
#
#   umbra_add_test(<name> SOURCES <file>... [DEPENDS <module>...]
#                  [LABELS <label>...])
#
# Adds a GoogleTest executable registered with CTest, its tests carrying the
# given CTest labels. Its main() configures the floating-point state of the
# thread before running the tests. Tests that need a GPU carry the label
# "gpu": the test presets leave them out, and
# `python3 -m forge check --label gpu` runs only them (see CMakePresets.json).
#
#   umbra_add_benchmark(<name> SOURCES <file>... [DEPENDS <module>...])
#
# Adds a Google Benchmark executable. Its main() configures the
# floating-point state of the thread before running the benchmarks.

include_guard(GLOBAL)

find_package(TBB 2023 CONFIG REQUIRED COMPONENTS tbb)

set(UMBRA_MODULE_INCLUDE_ROOT "${PROJECT_BINARY_DIR}/module-include")

# Applies the project settings to a target.
function(umbra_apply_settings target)
  umbra_apply_compiler_settings(${target})
  umbra_apply_warnings(${target})
  umbra_apply_safety(${target})
  umbra_apply_sanitizers(${target})
  umbra_apply_hardening(${target})
  umbra_apply_coverage(${target})
endfunction()

# Converts module names to their targets.
function(umbra_module_targets out)
  list(TRANSFORM ARGN PREPEND "umbra::" OUTPUT_VARIABLE targets)
  set(${out} ${targets} PARENT_SCOPE)
endfunction()

# Defines a static module library. <header_dir> is the module's directory
# under include/umbra/; it is exposed alone, through a link under the build
# tree, so that no other module's headers are visible by accident.
function(umbra_define_module target alias header_dir)
  cmake_parse_arguments(PARSE_ARGV 3 arg "" "" "SOURCES;DEPENDS;LINK")
  set(include_root "${UMBRA_MODULE_INCLUDE_ROOT}/${target}")
  set(include_link "${include_root}/umbra/${header_dir}")
  cmake_path(GET include_link PARENT_PATH include_parent)
  file(MAKE_DIRECTORY "${include_parent}")
  file(CREATE_LINK "${PROJECT_SOURCE_DIR}/include/umbra/${header_dir}"
       "${include_link}" SYMBOLIC)

  add_library(${target} STATIC ${arg_SOURCES})
  add_library(${alias} ALIAS ${target})
  target_include_directories(${target} PUBLIC "${include_root}")
  umbra_module_targets(dependencies ${arg_DEPENDS})
  target_link_libraries(${target} PUBLIC ${dependencies} TBB::tbb ${arg_LINK})
  umbra_apply_settings(${target})
endfunction()

# Builds a target with exceptions and RTTI. The library's classes are those
# of its shared objects, so the target keeps their default visibility (see
# UmbraCompiler.cmake).
function(umbra_enable_exceptions target)
  target_compile_options(${target} PRIVATE -fexceptions -frtti
                         $<$<CONFIG:Release>:-fvisibility=default>)
endfunction()

function(umbra_add_module module)
  if(module MATCHES "^ext(/|$)")
    message(FATAL_ERROR "use umbra_add_ext_module for bridge modules")
  endif()
  cmake_parse_arguments(PARSE_ARGV 1 arg "EXCEPTIONS" "" "SOURCES;DEPENDS;LINK")
  string(REPLACE "/" "_" name "${module}")
  string(REPLACE "/" "::" alias "${module}")
  umbra_define_module(umbra_${name} umbra::${alias} ${module}
                      SOURCES ${arg_SOURCES} DEPENDS ${arg_DEPENDS}
                      LINK ${arg_LINK})
  if(arg_EXCEPTIONS)
    umbra_enable_exceptions(umbra_${name})
  endif()
endfunction()

function(umbra_add_ext_module dependency)
  cmake_path(GET CMAKE_CURRENT_SOURCE_DIR PARENT_PATH parent)
  if(NOT parent STREQUAL "${PROJECT_SOURCE_DIR}/src/ext")
    message(FATAL_ERROR "bridge modules live in src/ext/<dependency>")
  endif()
  set(target umbra_ext_${dependency})
  umbra_define_module(${target} umbra::ext::${dependency} ext/${dependency}
                      ${ARGN})
  umbra_enable_exceptions(${target})
endfunction()

function(umbra_add_loadable_module target)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;HEADERS;LINK")
  add_library(${target} MODULE ${arg_SOURCES})
  foreach(module IN LISTS arg_HEADERS)
    target_include_directories(${target} PRIVATE
                               "${UMBRA_MODULE_INCLUDE_ROOT}/umbra_${module}")
  endforeach()
  target_link_libraries(${target} PRIVATE TBB::tbb ${arg_LINK})
  set_target_properties(${target} PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
  # The sanitizer runtimes live in the executable, so a sanitized module has
  # undefined symbols until it is loaded.
  target_link_options(${target} PRIVATE
    -Wl,--exclude-libs,ALL
    $<$<NOT:$<BOOL:${UMBRA_SANITIZER}>>:-Wl,--no-undefined>)
  umbra_apply_settings(${target})
endfunction()

function(umbra_add_executable name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;DEPENDS")
  add_executable(${name} ${arg_SOURCES})
  umbra_module_targets(dependencies ${arg_DEPENDS})
  target_link_libraries(${name} PRIVATE ${dependencies})
  umbra_apply_settings(${name})
endfunction()

function(umbra_add_test name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;DEPENDS;LABELS")
  if(NOT TARGET umbra_test_main)
    find_package(GTest CONFIG REQUIRED)
    include(GoogleTest)
    add_library(umbra_test_main OBJECT "${PROJECT_SOURCE_DIR}/tests/main.cc")
    target_link_libraries(umbra_test_main PUBLIC GTest::gtest umbra::core)
    umbra_apply_settings(umbra_test_main)
  endif()
  umbra_add_executable(${name} SOURCES ${arg_SOURCES} DEPENDS ${arg_DEPENDS})
  target_link_libraries(${name} PRIVATE umbra_test_main)
  if(arg_LABELS)
    gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST
                         PROPERTIES LABELS "${arg_LABELS}")
  else()
    gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST)
  endif()
  umbra_add_coverage_object(${name})
endfunction()

function(umbra_add_benchmark name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;DEPENDS")
  if(NOT TARGET umbra_benchmark_main)
    find_package(benchmark CONFIG REQUIRED)
    add_library(umbra_benchmark_main OBJECT
                "${PROJECT_SOURCE_DIR}/benchmarks/main.cc")
    target_link_libraries(umbra_benchmark_main PUBLIC benchmark::benchmark
                                                      umbra::core)
    umbra_apply_settings(umbra_benchmark_main)
  endif()
  umbra_add_executable(${name} SOURCES ${arg_SOURCES} DEPENDS ${arg_DEPENDS})
  target_link_libraries(${name} PRIVATE umbra_benchmark_main)
endfunction()
