# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Initial cache for the two-stage LLVM toolchain build (cmake -C).
#
# Stage 1 is configured here and built by the superbuild's compiler (the
# apt.llvm.org clang in the image build). It is minimal: clang, lld and the
# runtimes, for the host target only, with defaults that make every program it
# links use libc++, compiler-rt and libunwind. Stage 1 is never installed.
#
# Stage 2 (BOOTSTRAP_* variables, forwarded without the prefix) is built by
# stage 1 against stage 1's libc++ and is what gets installed. It defaults to
# the same LLVM runtimes, so neither the toolchain nor anything it builds
# needs libstdc++, libgcc_s or GCC's libatomic.

set(umbra_triple x86_64-unknown-linux-gnu)
set(umbra_link_flags "-rtlib=compiler-rt -unwindlib=libunwind")

# Settings shared by both stages.
foreach(prefix "" BOOTSTRAP_)
  set(${prefix}CMAKE_BUILD_TYPE Release CACHE STRING "")
  set(${prefix}LLVM_HOST_TRIPLE ${umbra_triple} CACHE STRING "")
  set(${prefix}LLVM_DEFAULT_TARGET_TRIPLE ${umbra_triple} CACHE STRING "")
  set(${prefix}LLVM_ENABLE_PER_TARGET_RUNTIME_DIR ON CACHE BOOL "")
  set(${prefix}LLVM_ENABLE_ASSERTIONS OFF CACHE BOOL "")
  set(${prefix}LLVM_INCLUDE_TESTS OFF CACHE BOOL "")
  set(${prefix}LLVM_INCLUDE_EXAMPLES OFF CACHE BOOL "")
  set(${prefix}LLVM_INCLUDE_BENCHMARKS OFF CACHE BOOL "")
  set(${prefix}LLVM_INCLUDE_DOCS OFF CACHE BOOL "")
  set(${prefix}LLVM_ENABLE_BINDINGS OFF CACHE BOOL "")
  set(${prefix}LLVM_ENABLE_ZLIB FORCE_ON CACHE STRING "")
  set(${prefix}LLVM_ENABLE_ZSTD FORCE_ON CACHE STRING "")

  # Programs linked by the toolchain default to the LLVM runtimes and lld.
  set(${prefix}CLANG_DEFAULT_CXX_STDLIB libc++ CACHE STRING "")
  set(${prefix}CLANG_DEFAULT_RTLIB compiler-rt CACHE STRING "")
  set(${prefix}CLANG_DEFAULT_UNWINDLIB libunwind CACHE STRING "")
  set(${prefix}CLANG_DEFAULT_LINKER lld CACHE STRING "")
  set(${prefix}CLANG_DEFAULT_OBJCOPY llvm-objcopy CACHE STRING "")

  # Runtimes: no libgcc, libgcc_s or libatomic anywhere.
  set(${prefix}LIBUNWIND_USE_COMPILER_RT ON CACHE BOOL "")
  set(${prefix}LIBCXXABI_USE_COMPILER_RT ON CACHE BOOL "")
  set(${prefix}LIBCXXABI_USE_LLVM_UNWINDER ON CACHE BOOL "")
  set(${prefix}LIBCXX_USE_COMPILER_RT ON CACHE BOOL "")
  # Pre-seeded: otherwise libc++ links GCC's libatomic when it is installed.
  set(${prefix}LIBCXX_HAS_ATOMIC_LIB NO CACHE BOOL "")
  set(${prefix}COMPILER_RT_USE_BUILTINS_LIBRARY ON CACHE BOOL "")
  set(${prefix}COMPILER_RT_USE_LLVM_UNWINDER ON CACHE BOOL "")
  set(${prefix}COMPILER_RT_CXX_LIBRARY libcxx CACHE STRING "")
  set(${prefix}COMPILER_RT_BUILD_STANDALONE_LIBATOMIC ON CACHE BOOL "")
  set(${prefix}COMPILER_RT_USE_ATOMIC_LIBRARY ON CACHE BOOL "")
  set(${prefix}SANITIZER_CXX_ABI libc++ CACHE STRING "")
  set(${prefix}SANITIZER_CXX_ABI_INTREE ON CACHE BOOL "")
  set(${prefix}SANITIZER_TEST_CXX libc++ CACHE STRING "")
  set(${prefix}SANITIZER_TEST_CXX_INTREE ON CACHE BOOL "")
endforeach()

# Stage 1: throwaway bootstrap compiler. The superbuild's compiler links it, so
# it may use that compiler's default C++ library.
set(LLVM_ENABLE_PROJECTS "clang;lld" CACHE STRING "")
set(LLVM_ENABLE_RUNTIMES "compiler-rt;libcxx;libcxxabi;libunwind" CACHE STRING "")
set(LLVM_TARGETS_TO_BUILD Native CACHE STRING "")
set(LLVM_ENABLE_LLD ON CACHE BOOL "")
set(COMPILER_RT_BUILD_SANITIZERS OFF CACHE BOOL "")
set(COMPILER_RT_BUILD_XRAY OFF CACHE BOOL "")
set(COMPILER_RT_BUILD_LIBFUZZER OFF CACHE BOOL "")
set(COMPILER_RT_BUILD_PROFILE OFF CACHE BOOL "")
set(COMPILER_RT_BUILD_MEMPROF OFF CACHE BOOL "")
set(COMPILER_RT_BUILD_ORC OFF CACHE BOOL "")
set(COMPILER_RT_BUILD_CTX_PROFILE OFF CACHE BOOL "")
set(COMPILER_RT_BUILD_GWP_ASAN OFF CACHE BOOL "")

set(CLANG_ENABLE_BOOTSTRAP ON CACHE BOOL "")
# Stage 2 links stage 1's libc++, libc++abi and libunwind.
set(CLANG_BOOTSTRAP_EXTRA_DEPS runtimes CACHE STRING "")

# Stage 2: the installed toolchain.
set(BOOTSTRAP_LLVM_ENABLE_PROJECTS "clang;clang-tools-extra;lld;lldb;bolt" CACHE STRING "")
set(BOOTSTRAP_LLVM_ENABLE_RUNTIMES "compiler-rt;libcxx;libcxxabi;libunwind" CACHE STRING "")
set(BOOTSTRAP_LLVM_TARGETS_TO_BUILD "X86;NVPTX" CACHE STRING "")
set(BOOTSTRAP_LLVM_ENABLE_LIBCXX ON CACHE BOOL "")
set(BOOTSTRAP_LLVM_ENABLE_LLD ON CACHE BOOL "")
set(BOOTSTRAP_CMAKE_EXE_LINKER_FLAGS "${umbra_link_flags}" CACHE STRING "")
set(BOOTSTRAP_CMAKE_SHARED_LINKER_FLAGS "${umbra_link_flags}" CACHE STRING "")
set(BOOTSTRAP_CMAKE_MODULE_LINKER_FLAGS "${umbra_link_flags}" CACHE STRING "")

# Shared LLVM and Clang libraries: tools link them, and OSL will consume them.
set(BOOTSTRAP_LLVM_BUILD_LLVM_DYLIB ON CACHE BOOL "")
set(BOOTSTRAP_LLVM_LINK_LLVM_DYLIB ON CACHE BOOL "")
set(BOOTSTRAP_CLANG_LINK_CLANG_DYLIB ON CACHE BOOL "")

# Full install: every LLVM tool and utility, no toolchain-only trimming.
set(BOOTSTRAP_LLVM_INSTALL_UTILS ON CACHE BOOL "")

set(BOOTSTRAP_LLVM_ENABLE_LIBXML2 FORCE_ON CACHE STRING "")
set(BOOTSTRAP_LLVM_ENABLE_LIBEDIT ON CACHE BOOL "")
set(BOOTSTRAP_LLDB_ENABLE_PYTHON OFF CACHE BOOL "")
set(BOOTSTRAP_LLDB_ENABLE_LUA OFF CACHE BOOL "")
set(BOOTSTRAP_LLDB_ENABLE_LIBEDIT ON CACHE BOOL "")
set(BOOTSTRAP_LLDB_ENABLE_CURSES ON CACHE BOOL "")
set(BOOTSTRAP_LLDB_ENABLE_LZMA ON CACHE BOOL "")
set(BOOTSTRAP_LLDB_ENABLE_LIBXML2 ON CACHE BOOL "")
