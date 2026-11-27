# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Sanitizer builds of the test presets, selected by UMBRA_SANITIZER:
#
#   (empty)   no sanitizer: the debug and release presets;
#   address   the asan preset: AddressSanitizer (with LeakSanitizer) and
#             UndefinedBehaviorSanitizer, including the checks that plain
#             -fsanitize=undefined leaves out: unsigned overflow, implicit
#             integer conversions, unsigned shift base and nullability;
#   thread    the tsan preset: ThreadSanitizer, with the UndefinedBehavior-
#             Sanitizer integer checks (signed and unsigned overflow, shift,
#             integer divide-by-zero), implicit conversions, nullability
#             and float-cast overflow;
#   realtime  the rtsan preset: RealtimeSanitizer, which aborts when a
#             function declared [[clang::nonblocking]], or what it calls,
#             allocates, locks or makes a blocking system call, inside
#             third-party libraries too
#             (docs/adr/0003-no-allocations-on-the-hot-path.md).
#
# Every preset pattern-initializes locals, so reads of uninitialized memory
# produce recognizable garbage instead of zeros.
#
# Every check is fatal (no recovery), so a test that triggers one fails. The
# wrapping arithmetic helpers of include/umbra/core/wrapping.h are the only
# code exempt from the overflow checks.
#
# Every executable of a sanitizer build links sanitizers/default_options.cc,
# which sets the default runtime options (halt and abort on the first error,
# llvm-symbolizer for file:line reports, the TSan suppressions file). The
# environment variables (ASAN_OPTIONS, UBSAN_OPTIONS, TSAN_OPTIONS,
# RTSAN_OPTIONS) still
# override them.
#
# The settings apply through umbra_apply_sanitizers(<target>), called by the
# helpers in UmbraModules.cmake.
#
# The tsan preset finds oneTBB in <prefix>/tsan before <prefix>, for every
# prefix of CMAKE_PREFIX_PATH (the variable, then the environment): there the
# dependency build installs a libtbb instrumented with ThreadSanitizer, whose
# synchronization TSan can see. Include this module before UmbraModules.cmake,
# which finds oneTBB.

include_guard(GLOBAL)

set(UMBRA_SANITIZER "" CACHE STRING
    "Sanitizers: empty, address (ASan+UBSan), thread (TSan), realtime (RTSan)")
set_property(CACHE UMBRA_SANITIZER PROPERTY STRINGS "" address thread realtime)

if(UMBRA_SANITIZER STREQUAL "address")
  set(UMBRA_SANITIZE_FLAGS
    -fsanitize=address,undefined,integer,implicit-conversion,nullability,float-cast-overflow
    -fno-sanitize-recover=all)
elseif(UMBRA_SANITIZER STREQUAL "thread")
  set(UMBRA_SANITIZE_FLAGS
    -fsanitize=thread,integer,implicit-conversion,nullability,float-cast-overflow
    -fno-sanitize-recover=all)
  set(umbra_tsan_prefixes "")
  # The environment variable separates its prefixes with ':'.
  string(REPLACE ":" ";" umbra_env_prefixes "$ENV{CMAKE_PREFIX_PATH}")
  foreach(prefix IN LISTS CMAKE_PREFIX_PATH umbra_env_prefixes)
    if(IS_DIRECTORY "${prefix}/tsan")
      list(APPEND umbra_tsan_prefixes "${prefix}/tsan")
    endif()
  endforeach()
  if(NOT umbra_tsan_prefixes)
    message(FATAL_ERROR
            "UMBRA_SANITIZER=thread needs the ThreadSanitizer oneTBB in "
            "<prefix>/tsan for a prefix of CMAKE_PREFIX_PATH")
  endif()
  list(PREPEND CMAKE_PREFIX_PATH ${umbra_tsan_prefixes})
elseif(UMBRA_SANITIZER STREQUAL "realtime")
  set(UMBRA_SANITIZE_FLAGS -fsanitize=realtime)
elseif(NOT UMBRA_SANITIZER STREQUAL "")
  message(FATAL_ERROR
          "UMBRA_SANITIZER must be empty, address, thread or realtime "
          "(found '${UMBRA_SANITIZER}')")
endif()

if(UMBRA_SANITIZER)
  set(UMBRA_SANITIZE_COMPILE_FLAGS
    -ftrivial-auto-var-init=pattern -fno-omit-frame-pointer)
  # The symbolizer that sits next to the compiler, as the LLVM binary tools
  # in UmbraCompiler.cmake.
  find_program(UMBRA_LLVM_SYMBOLIZER NAMES llvm-symbolizer
               HINTS "${umbra_compiler_dir}" "${umbra_compiler_real_dir}"
               REQUIRED)
endif()

# Adds the sanitizer default options to an executable.
function(umbra_add_sanitizer_options target)
  if(NOT TARGET umbra_sanitizer_options)
    add_library(umbra_sanitizer_options OBJECT
                "${PROJECT_SOURCE_DIR}/sanitizers/default_options.cc")
    target_compile_definitions(umbra_sanitizer_options PRIVATE
      UMBRA_LLVM_SYMBOLIZER="${UMBRA_LLVM_SYMBOLIZER}"
      UMBRA_TSAN_SUPPRESSIONS="${PROJECT_SOURCE_DIR}/sanitizers/tsan.supp")
    umbra_apply_settings(umbra_sanitizer_options)
  endif()
  target_link_libraries(${target} PRIVATE umbra_sanitizer_options)
endfunction()

function(umbra_apply_sanitizers target)
  if(NOT UMBRA_SANITIZER)
    return()
  endif()
  target_compile_options(${target} PRIVATE
    ${UMBRA_SANITIZE_FLAGS} ${UMBRA_SANITIZE_COMPILE_FLAGS})
  target_link_options(${target} PRIVATE ${UMBRA_SANITIZE_FLAGS})
  get_target_property(type ${target} TYPE)
  if(type STREQUAL "EXECUTABLE")
    umbra_add_sanitizer_options(${target})
  endif()
endfunction()
