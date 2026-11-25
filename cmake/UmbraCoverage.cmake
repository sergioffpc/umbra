# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Source-based code coverage of the tests, for local reports only (the
# coverage preset, `python3 -m forge coverage`). There is no coverage gate and
# CI does not run it.
#
# With UMBRA_COVERAGE on, every umbra target is instrumented
# (-fprofile-instr-generate -fcoverage-mapping). The coverage test preset sets
# LLVM_PROFILE_FILE so that each test process writes a raw profile to
# <build>/profiles/, and the coverage-report target merges them with
# llvm-profdata and writes the llvm-cov reports of umbra's own sources (tests
# excluded) to <build>/report/: summary.txt and html/index.html. See
# GenerateCoverageReport.cmake.
#
# The settings apply through umbra_apply_coverage(<target>), called by the
# helpers in UmbraModules.cmake; umbra_add_coverage_object(<target>) adds a
# test executable to the report.

include_guard(GLOBAL)

option(UMBRA_COVERAGE "Instrument the build for source-based code coverage" OFF)

if(UMBRA_COVERAGE)
  # The profile tools that sit next to the compiler, as the LLVM binary tools
  # in UmbraCompiler.cmake.
  foreach(umbra_tool IN ITEMS profdata cov)
    string(TOUPPER "${umbra_tool}" umbra_tool_var)
    find_program(UMBRA_LLVM_${umbra_tool_var} NAMES llvm-${umbra_tool}
                 HINTS "${umbra_compiler_dir}" "${umbra_compiler_real_dir}"
                 REQUIRED)
  endforeach()
endif()

function(umbra_apply_coverage target)
  if(NOT UMBRA_COVERAGE)
    return()
  endif()
  target_compile_options(${target} PRIVATE
    -fprofile-instr-generate -fcoverage-mapping)
  target_link_options(${target} PRIVATE -fprofile-instr-generate)
endfunction()

function(umbra_add_coverage_object target)
  if(UMBRA_COVERAGE)
    set_property(GLOBAL APPEND PROPERTY UMBRA_COVERAGE_OBJECTS ${target})
  endif()
endfunction()

# Defines the coverage-report target once every test is known.
function(umbra_define_coverage_report)
  get_property(targets GLOBAL PROPERTY UMBRA_COVERAGE_OBJECTS)
  # "|"-separated, so that the list stays one argument of the command.
  set(objects "")
  foreach(target IN LISTS targets)
    list(APPEND objects "$<TARGET_FILE:${target}>")
  endforeach()
  list(JOIN objects "|" objects)
  add_custom_target(coverage-report
    COMMAND "${CMAKE_COMMAND}"
            "-DPROFDATA=${UMBRA_LLVM_PROFDATA}"
            "-DCOV=${UMBRA_LLVM_COV}"
            "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DPROFILE_DIR=${PROJECT_BINARY_DIR}/profiles"
            "-DREPORT_DIR=${PROJECT_BINARY_DIR}/report"
            "-DOBJECTS=${objects}"
            -P "${PROJECT_SOURCE_DIR}/cmake/GenerateCoverageReport.cmake"
    DEPENDS ${targets}
    COMMENT "Generating the coverage report"
    VERBATIM)
endfunction()

if(UMBRA_COVERAGE)
  cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}"
                 CALL umbra_define_coverage_report)
endif()
