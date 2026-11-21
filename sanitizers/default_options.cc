// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// Default runtime options of the sanitizers, linked into every executable of
// a sanitizer build (see cmake/UmbraSanitizers.cmake). Each runtime calls its
// hook at startup; ASAN_OPTIONS, UBSAN_OPTIONS and TSAN_OPTIONS in the
// environment override these defaults.
//
// Every runtime stops at the first error and aborts, so a test that triggers
// a report fails, and symbolizes its reports (file:line) with the toolchain's
// llvm-symbolizer. UMBRA_LLVM_SYMBOLIZER and UMBRA_TSAN_SUPPRESSIONS are
// absolute paths defined by cmake/UmbraSanitizers.cmake.

#include <sanitizer/asan_interface.h>
#include <sanitizer/tsan_interface.h>
#include <sanitizer/ubsan_interface.h>

#define UMBRA_SANITIZER_COMMON_OPTIONS           \
  "halt_on_error=1:abort_on_error=1:symbolize=1" \
  ":external_symbolizer_path=" UMBRA_LLVM_SYMBOLIZER

// The hooks are declared by the sanitizer interface headers above, which fix
// their names.

// AddressSanitizer and its LeakSanitizer: also detect stack use after
// return, initialization-order bugs between translation units, and string
// functions reading past the terminator.
extern "C" const char* __asan_default_options() {
  return UMBRA_SANITIZER_COMMON_OPTIONS
      ":detect_leaks=1:detect_stack_use_after_return=1"
      ":check_initialization_order=1:strict_init_order=1"
      ":strict_string_checks=1";
}

// UndefinedBehaviorSanitizer: print the stack of every report.
extern "C" const char* __ubsan_default_options() {
  return UMBRA_SANITIZER_COMMON_OPTIONS ":print_stacktrace=1";
}

// ThreadSanitizer: report both stacks of a lock-order inversion, and read
// the justified suppressions of uninstrumented libraries.
extern "C" const char* __tsan_default_options() {
  return UMBRA_SANITIZER_COMMON_OPTIONS
      ":second_deadlock_stack=1:suppressions=" UMBRA_TSAN_SUPPRESSIONS;
}
