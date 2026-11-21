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
#             and float-cast overflow.
#
# Both presets pattern-initialize locals, so reads of uninitialized memory
# produce recognizable garbage instead of zeros.
#
# Every check is fatal (no recovery), so a test that triggers one fails. The
# wrapping arithmetic helpers of include/umbra/core/wrapping.h are the only
# code exempt from the overflow checks.
#
# Every executable of a sanitizer build links sanitizers/default_options.cc,
# which sets the default runtime options (halt and abort on the first error,
# llvm-symbolizer for file:line reports, the TSan suppressions file). The
# environment variables (ASAN_OPTIONS, UBSAN_OPTIONS, TSAN_OPTIONS) still
# override them.
#
# The settings apply through umbra_apply_sanitizers(<target>), called by the
# helpers in UmbraModules.cmake.

include_guard(GLOBAL)

set(UMBRA_SANITIZER "" CACHE STRING
    "Sanitizers of the build: empty, address (ASan+UBSan) or thread (TSan)")
set_property(CACHE UMBRA_SANITIZER PROPERTY STRINGS "" address thread)

if(UMBRA_SANITIZER STREQUAL "address")
  set(UMBRA_SANITIZE_FLAGS
    -fsanitize=address,undefined,integer,implicit-conversion,nullability,float-cast-overflow
    -fno-sanitize-recover=all)
elseif(UMBRA_SANITIZER STREQUAL "thread")
  set(UMBRA_SANITIZE_FLAGS
    -fsanitize=thread,integer,implicit-conversion,nullability,float-cast-overflow
    -fno-sanitize-recover=all)
elseif(NOT UMBRA_SANITIZER STREQUAL "")
  message(FATAL_ERROR
          "UMBRA_SANITIZER must be empty, address or thread "
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
