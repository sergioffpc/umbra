# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Script run by the coverage-report target (UmbraCoverage.cmake):
#
#   cmake -DPROFDATA=<llvm-profdata> -DCOV=<llvm-cov> -DSOURCE_DIR=<dir>
#         -DPROFILE_DIR=<dir> -DREPORT_DIR=<dir> -DOBJECTS=<exe>|<exe>...
#         -P GenerateCoverageReport.cmake
#
# Merges the raw profiles of PROFILE_DIR into REPORT_DIR/umbra.profdata,
# deletes them (so the next test run starts afresh), and writes the reports of
# umbra's own sources: the summary (printed and in REPORT_DIR/summary.txt) and
# the annotated sources (REPORT_DIR/html/index.html).

cmake_minimum_required(VERSION 4.2)

file(GLOB raw_profiles "${PROFILE_DIR}/*.profraw")
if(NOT raw_profiles)
  message(FATAL_ERROR
          "no raw profiles in ${PROFILE_DIR}: run the tests first "
          "(ctest --preset coverage, or python3 -m forge coverage)")
endif()

file(MAKE_DIRECTORY "${REPORT_DIR}")
set(profile "${REPORT_DIR}/umbra.profdata")
execute_process(
  COMMAND "${PROFDATA}" merge -sparse ${raw_profiles} -o "${profile}"
  COMMAND_ERROR_IS_FATAL ANY)
file(REMOVE ${raw_profiles})

# llvm-cov takes the first executable as is and the others after -object.
string(REPLACE "|" ";" objects "${OBJECTS}")
list(POP_FRONT objects first_object)
list(TRANSFORM objects PREPEND "-object=")
# Only umbra's sources: not the tests, the build tree (generated sources) or
# the dependencies.
string(REGEX REPLACE "([][.*+?^$()|\\])" "\\\\\\1" source_regex
       "${SOURCE_DIR}")
set(options
  "-instr-profile=${profile}"
  "-ignore-filename-regex=^(${source_regex}/(tests|build)/|/opt/|/usr/)")

execute_process(
  COMMAND "${COV}" report "${first_object}" ${objects} ${options}
  OUTPUT_FILE "${REPORT_DIR}/summary.txt"
  COMMAND_ERROR_IS_FATAL ANY)
file(READ "${REPORT_DIR}/summary.txt" summary)
message("${summary}")

file(REMOVE_RECURSE "${REPORT_DIR}/html")
execute_process(
  COMMAND "${COV}" show "${first_object}" ${objects} ${options}
          -format=html "-output-dir=${REPORT_DIR}/html"
          -show-line-counts-or-regions -show-branches=count
  COMMAND_ERROR_IS_FATAL ANY)
message("Coverage report: ${REPORT_DIR}/html/index.html")
