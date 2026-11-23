# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Layer "toolchain": LLVM built from source in two stages (see
# config/llvm-project.cmake). Configure the dependency build with the stage-1
# compiler, e.g. CC=clang-23 CXX=clang++-23 from apt.llvm.org.

set(UMBRA_LLVM_PARALLEL_LINK_JOBS 2 CACHE STRING
    "Concurrent link jobs in the LLVM build; links of the shared libraries need several GB each")

set(umbra_llvm_triple x86_64-unknown-linux-gnu)
file(STRINGS "${UMBRA_THIRD_PARTY_DIR}/llvm-project/cmake/Modules/LLVMVersion.cmake"
     umbra_llvm_major REGEX "set\\(LLVM_VERSION_MAJOR [0-9]+\\)")
string(REGEX MATCH "[0-9]+" umbra_llvm_major "${umbra_llvm_major}")
# libc++, libc++abi and libunwind; compiler-rt's shared runtimes (sanitizers,
# libclang_rt.atomic).
set(umbra_llvm_runtime_dirs
    "${CMAKE_INSTALL_PREFIX}/lib/${umbra_llvm_triple}"
    "${CMAKE_INSTALL_PREFIX}/lib/clang/${umbra_llvm_major}/lib/${umbra_llvm_triple}")

umbra_add_dependency(llvm-project
  SOURCE_SUBDIR llvm
  CMAKE_ARGS
    -C "${CMAKE_CURRENT_LIST_DIR}/../config/llvm-project.cmake"
    "-DLLVM_PARALLEL_LINK_JOBS=${UMBRA_LLVM_PARALLEL_LINK_JOBS}"
    "-DBOOTSTRAP_LLVM_PARALLEL_LINK_JOBS=${UMBRA_LLVM_PARALLEL_LINK_JOBS}"
  # Stage-2 tools run during the build against stage 1's shared libc++.
  BUILD_ENVIRONMENT "LD_LIBRARY_PATH=<BINARY_DIR>/lib/${umbra_llvm_triple}"
  BUILD_TARGETS stage2
  INSTALL_TARGETS stage2-install)

# The runtime directories are per target, and the dynamic loader does not
# search them by default. The staged tree carries an ld.so.conf.d entry for
# them; run ldconfig after copying it to /.
if(UMBRA_STAGING_DIR)
  set(umbra_ld_conf "${CMAKE_CURRENT_BINARY_DIR}/umbra-llvm-runtimes.conf")
  list(JOIN umbra_llvm_runtime_dirs "\n" umbra_ld_conf_lines)
  file(WRITE "${umbra_ld_conf}" "${umbra_ld_conf_lines}\n")
  ExternalProject_Add_Step(llvm-project ld-so-conf
    COMMAND ${CMAKE_COMMAND} -E copy "${umbra_ld_conf}"
            "${UMBRA_STAGING_DIR}/etc/ld.so.conf.d/umbra-llvm-runtimes.conf"
    DEPENDEES install
    COMMENT "Staging the ld.so.conf.d entry for the LLVM runtimes")
endif()

umbra_add_layer(toolchain DEPENDENCIES llvm-project)
