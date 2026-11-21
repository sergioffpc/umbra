# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Builds one static clangd index from several compile databases: merges them
# into WORK_DIR/compile_commands.json and runs clangd-indexer over every
# translation unit. The index records absolute source paths, so it is only
# valid where the sources stay at the path they were compiled from.
#
# Run as:
#   cmake -DINDEXER=<clangd-indexer> -DDATABASES=<db>|<db>... \
#         -DWORK_DIR=<dir> -DOUTPUT=<index file> -P BuildClangdIndex.cmake

cmake_minimum_required(VERSION 4.2)

foreach(var IN ITEMS INDEXER DATABASES WORK_DIR OUTPUT)
  if(NOT ${var})
    message(FATAL_ERROR "BuildClangdIndex: ${var} is not set")
  endif()
endforeach()

# Every database is a JSON array of objects as CMake writes it; the merged
# database is the concatenation of their elements.
#
# Plain string operations only: a compile command may contain ";", which CMake
# lists would split.
string(REPLACE "|" ";" databases "${DATABASES}")
set(entries "")
foreach(database IN LISTS databases)
  if(NOT EXISTS "${database}")
    message(FATAL_ERROR "BuildClangdIndex: '${database}' does not exist")
  endif()
  file(READ "${database}" content)
  string(STRIP "${content}" content)
  string(LENGTH "${content}" length)
  math(EXPR last "${length} - 1")
  if(length LESS 2)
    message(FATAL_ERROR "BuildClangdIndex: '${database}' is not a JSON array")
  endif()
  string(SUBSTRING "${content}" 0 1 first_char)
  string(SUBSTRING "${content}" ${last} 1 last_char)
  if(NOT first_char STREQUAL "[" OR NOT last_char STREQUAL "]")
    message(FATAL_ERROR "BuildClangdIndex: '${database}' is not a JSON array")
  endif()
  math(EXPR inner_length "${length} - 2")
  string(SUBSTRING "${content}" 1 ${inner_length} content)
  string(STRIP "${content}" content)
  if(content STREQUAL "")
    continue()
  endif()
  if(NOT entries STREQUAL "")
    string(APPEND entries ",\n")
  endif()
  string(APPEND entries "${content}")
endforeach()
if(entries STREQUAL "")
  message(FATAL_ERROR "BuildClangdIndex: the compile databases are empty")
endif()
file(MAKE_DIRECTORY "${WORK_DIR}")
file(WRITE "${WORK_DIR}/compile_commands.json" "[\n${entries}\n]\n")

cmake_path(GET OUTPUT PARENT_PATH output_dir)
file(MAKE_DIRECTORY "${output_dir}")
execute_process(
  COMMAND "${INDEXER}" --executor=all-TUs "${WORK_DIR}/compile_commands.json"
  OUTPUT_FILE "${OUTPUT}.tmp"
  RESULT_VARIABLE result)
file(SIZE "${OUTPUT}.tmp" size)
if(NOT result EQUAL 0 OR size EQUAL 0)
  file(REMOVE "${OUTPUT}.tmp")
  message(FATAL_ERROR "BuildClangdIndex: clangd-indexer failed (${result})")
endif()
file(RENAME "${OUTPUT}.tmp" "${OUTPUT}")
message(STATUS "BuildClangdIndex: wrote ${OUTPUT} (${size} bytes)")
