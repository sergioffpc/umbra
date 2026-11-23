# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# CUDA and OptiX device code, for the GPU module (libumbra-gpu.so).
#
# UMBRA_GPU selects whether the GPU module is built:
#
#   AUTO  (default) when nvcc and the OptiX headers are found: in the
#         devcontainer and CI, whose image carries them under /opt/cuda;
#   ON    always, failing when they are missing;
#   OFF   never: umbra then builds with permissive dependencies only.
#
# UMBRA_GPU_ENABLED tells the rest of the build whether it is on.
#
# Device code (CUDA kernels, OptiX programs) is the one exception to the
# LLVM-only toolchain: nvcc compiles it, for sm_120 only, with clang and libc++
# as its host compiler. nvcc 13.5 supports clang up to 22, hence
# -allow-unsupported-compiler for clang 23. Host code, the module's included,
# stays clang + libc++ + lld, and calls the CUDA runtime API through
# cudart_static (CUDA::cudart_static).
#
#   umbra_add_optix_ir(<target> SOURCES <file.cu>... FUNCTION <name>
#                      NAMESPACE <namespace>)
#
# Compiles OptiX programs to OptiX-IR and adds to <target> a generated source
# that defines `std::span<const char> <namespace>::<name>()`, returning
# the IR of the programs, linked into one module per source; with several
# sources, only the first is returned. Debug builds compile with -G (source
# level debugging in Nsight; the module must then be created with
# OPTIX_COMPILE_DEBUG_LEVEL_FULL), the others with -lineinfo. Code that
# creates OptiX modules sees UMBRA_GPU_DEVICE_DEBUG=1 in Debug builds.

include_guard(GLOBAL)

set(UMBRA_GPU "AUTO" CACHE STRING
    "Build the GPU module (libumbra-gpu.so): AUTO, ON or OFF")
set_property(CACHE UMBRA_GPU PROPERTY STRINGS AUTO ON OFF)
if(NOT UMBRA_GPU MATCHES "^(AUTO|ON|OFF)$")
  message(FATAL_ERROR "UMBRA_GPU must be AUTO, ON or OFF (found '${UMBRA_GPU}')")
endif()

set(UMBRA_GPU_ENABLED OFF)
if(NOT UMBRA_GPU STREQUAL "OFF")
  find_program(UMBRA_NVCC NAMES nvcc
               HINTS "$ENV{CUDAToolkit_ROOT}/bin" /opt/cuda/bin)
  find_path(UMBRA_OPTIX_INCLUDE_DIR NAMES optix.h
            HINTS "$ENV{OptiX_INSTALL_DIR}/include" /opt/cuda/optix/include)
  if(UMBRA_NVCC AND UMBRA_OPTIX_INCLUDE_DIR)
    set(UMBRA_GPU_ENABLED ON)
  elseif(UMBRA_GPU STREQUAL "ON")
    message(FATAL_ERROR
            "UMBRA_GPU is ON but nvcc (${UMBRA_NVCC}) or the OptiX headers "
            "(${UMBRA_OPTIX_INCLUDE_DIR}) are missing; build in the "
            "devcontainer")
  else()
    message(STATUS "GPU module: off (no nvcc or OptiX headers)")
  endif()
endif()

if(UMBRA_GPU_ENABLED)
  set(CMAKE_CUDA_COMPILER "${UMBRA_NVCC}")
  set(CMAKE_CUDA_HOST_COMPILER "${CMAKE_CXX_COMPILER}")
  set(CMAKE_CUDA_FLAGS_INIT
      "-allow-unsupported-compiler -Xcompiler=-stdlib=libc++")
  # SASS only: no PTX fallback for other architectures.
  set(CMAKE_CUDA_ARCHITECTURES 120-real)
  set(CMAKE_CUDA_STANDARD 20)
  set(CMAKE_CUDA_STANDARD_REQUIRED ON)
  set(CMAKE_CUDA_EXTENSIONS OFF)
  enable_language(CUDA)
  find_package(CUDAToolkit REQUIRED)

  add_library(umbra_optix INTERFACE)
  add_library(umbra::optix ALIAS umbra_optix)
  target_include_directories(umbra_optix SYSTEM INTERFACE
                             "${UMBRA_OPTIX_INCLUDE_DIR}")

  message(STATUS "GPU module: on (nvcc ${CMAKE_CUDA_COMPILER_VERSION}, "
                 "OptiX headers in ${UMBRA_OPTIX_INCLUDE_DIR})")
endif()

set(UMBRA_EMBED_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/GenerateEmbeddedData.cmake")

function(umbra_add_optix_ir target)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "FUNCTION;NAMESPACE" "SOURCES")
  set(programs ${target}_${arg_FUNCTION}_optixir)
  add_library(${programs} OBJECT ${arg_SOURCES})
  set_target_properties(${programs} PROPERTIES CUDA_OPTIX_COMPILATION ON)
  target_link_libraries(${programs} PRIVATE umbra::optix)
  target_include_directories(${programs} PRIVATE
                             "${PROJECT_SOURCE_DIR}/include")
  target_compile_options(${programs} PRIVATE
    $<IF:$<CONFIG:Debug>,-G,-lineinfo>)

  set(output "${CMAKE_CURRENT_BINARY_DIR}/${arg_FUNCTION}.cc")
  add_custom_command(
    OUTPUT "${output}"
    COMMAND "${CMAKE_COMMAND}"
            "-DINPUT=$<TARGET_OBJECTS:${programs}>"
            "-DOUTPUT=${output}"
            "-DFUNCTION=${arg_FUNCTION}"
            "-DNAMESPACE=${arg_NAMESPACE}"
            -P "${UMBRA_EMBED_SCRIPT}"
    DEPENDS "$<TARGET_OBJECTS:${programs}>" "${UMBRA_EMBED_SCRIPT}"
    COMMENT "Embedding the OptiX-IR of ${arg_FUNCTION}"
    VERBATIM)
  target_sources(${target} PRIVATE "${output}")
  target_compile_definitions(${target} PRIVATE
    UMBRA_GPU_DEVICE_DEBUG=$<IF:$<CONFIG:Debug>,1,0>)
endfunction()
