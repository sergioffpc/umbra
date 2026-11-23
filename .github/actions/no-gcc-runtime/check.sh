#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Usage: check.sh PATH...
#
# Fails when an ELF file under the given files or directories needs a GCC
# runtime (libstdc++, libgcc_s or GCC's libatomic), directly (NEEDED) or
# through its dependencies (ldd), or has a dependency that does not resolve,
# and when there is no ELF file at all (a wrong path). Needs llvm-readelf and
# ldd. This is the single definition of the rule: the no-gcc-runtime action
# runs it on the build trees and in the images, and the toolchain
# verification step of the deps workflow runs it on its test programs.
set -euo pipefail

# compiler-rt's own atomics library is libclang_rt.atomic.so, which this does
# not match.
gcc_runtime='libstdc\+\+\.so|libgcc_s\.so|(^|[^_.[:alnum:]])libatomic\.so'
status=0
fail() {
  echo "::error::$*"
  status=1
}

elf_count=0
while IFS= read -r -d '' file; do
  magic=$(head -c 4 "$file" | od -An -tx1 | tr -d ' \n')
  [[ "$magic" == 7f454c46 ]] || continue
  elf_count=$((elf_count + 1))
  if llvm-readelf --dynamic "$file" 2>/dev/null |
    grep '(NEEDED)' | grep -Eq "$gcc_runtime"; then
    fail "$file needs a GCC runtime (NEEDED)"
  fi
  # ldd fails on files that are not dynamically linked (objects, archives).
  if deps=$(ldd "$file" 2>/dev/null); then
    if grep -Eq "$gcc_runtime" <<<"$deps"; then
      fail "$file loads a GCC runtime:" \
        "$(grep -E "$gcc_runtime" <<<"$deps" | xargs)"
    fi
    if grep -q 'not found' <<<"$deps"; then
      fail "$file has unresolved libraries:" \
        "$(grep 'not found' <<<"$deps" | xargs)"
    fi
  fi
done < <(find "$@" -type f -print0)

if ((elf_count == 0)); then
  fail "no ELF file under $*"
fi
echo "Checked $elf_count ELF files under $*."
exit "$status"
