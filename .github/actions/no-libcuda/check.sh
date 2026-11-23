#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Usage: check.sh PATH...
#
# Fails when an ELF file under the given files or directories has the CUDA
# driver, libcuda.so.1, among its NEEDED entries, unless it is a module loaded
# at run time (RUNTIME_MODULES below): umbra and its dependencies run on hosts
# without an NVIDIA driver, and only those modules, loaded with dlopen when a
# usable GPU exists, may need it. Also fails when there is no ELF file at all
# (a wrong path). Needs llvm-readelf. This is the single definition of the
# rule: the no-libcuda action runs it on the build trees and in the images.
set -euo pipefail

# File name patterns of the modules loaded at run time: umbra's GPU module
# and OIDN's GPU devices.
RUNTIME_MODULES=(
  'libumbra-gpu.so'
  'libOpenImageDenoise_device_*.so*'
)

status=0
fail() {
  echo "::error::$*"
  status=1
}

is_runtime_module() {
  local name pattern
  name=$(basename "$1")
  for pattern in "${RUNTIME_MODULES[@]}"; do
    # The pattern is a glob on purpose.
    # shellcheck disable=SC2053
    [[ "$name" == $pattern ]] && return 0
  done
  return 1
}

elf_count=0
while IFS= read -r -d '' file; do
  magic=$(head -c 4 "$file" | od -An -tx1 | tr -d ' \n')
  [[ "$magic" == 7f454c46 ]] || continue
  elf_count=$((elf_count + 1))
  if llvm-readelf --dynamic "$file" 2>/dev/null |
    grep '(NEEDED)' | grep -Eq '\[libcuda\.so(\.[0-9]+)*\]'; then
    if is_runtime_module "$file"; then
      echo "$file needs libcuda.so.1 (a module loaded at run time)"
    else
      fail "$file needs libcuda.so.1; only modules loaded at run time may"
    fi
  fi
done < <(find "$@" -type f -print0)

if ((elf_count == 0)); then
  fail "no ELF file under $*"
fi
echo "Checked $elf_count ELF files under $* for libcuda.so.1."
exit "$status"
