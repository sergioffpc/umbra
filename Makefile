# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Thin wrapper over the CMake presets (CMakePresets.json) for daily use and
# CI. Run inside the devcontainer.
#
# Targets prefixed with __ are the steps of a public target that CI runs as
# separate, parallel jobs; `make help` leaves them out.

.DEFAULT_GOAL := all
.PHONY: all test release sanitizers coverage format checks clean help \
	__asan __tsan __validators __format-check __lint

all: ## Debug build
	cmake --preset debug
	cmake --build --preset debug

# LABEL=<regex> runs only the tests with a matching CTest label, e.g.
# `make test LABEL=gpu` on a GPU host.
test: all ## Debug build and run the tests (without the GPU tests; LABEL=gpu for only those)
ifeq ($(LABEL),)
	ctest --preset debug
else
	ctest --test-dir build/debug --output-on-failure -L '$(LABEL)'
endif

release: ## Release build (LTO) and run the tests
	cmake --workflow --preset release

sanitizers: __asan __tsan ## ASan + UBSan, then TSan + UBSan integer checks: build and run the tests (LABEL=gpu: the GPU tests, also under compute-sanitizer)

# LABEL=<regex> also selects the tests of the sanitizer builds, and runs them
# under NVIDIA's compute-sanitizer too, which checks the kernels they launch:
# memcheck (out-of-bounds and misaligned device accesses, device memory
# leaks) and initcheck (reads of uninitialized device memory) with asan,
# racecheck (shared-memory races) and synccheck (misused barriers) with tsan.
# For example `make sanitizers LABEL=gpu` on a GPU host.
#
# AddressSanitizer keeps the shadow gap of the address space unmapped, where
# the CUDA driver maps device memory, so that cudaMalloc would fail with out
# of memory: protect_shadow_gap=0 lets the driver map it.
GPU_SANITIZER = compute-sanitizer --error-exitcode 1 --target-processes all
ASAN_GPU_OPTIONS = protect_shadow_gap=0

__asan:
ifeq ($(LABEL),)
	cmake --workflow --preset asan
else
	cmake --preset asan
	cmake --build --preset asan
	ASAN_OPTIONS=$(ASAN_GPU_OPTIONS) $(GPU_SANITIZER) --tool memcheck --leak-check full \
	  ctest --test-dir build/asan --output-on-failure -L '$(LABEL)'
	ASAN_OPTIONS=$(ASAN_GPU_OPTIONS) $(GPU_SANITIZER) --tool initcheck \
	  ctest --test-dir build/asan --output-on-failure -L '$(LABEL)'
endif

__tsan:
ifeq ($(LABEL),)
	cmake --workflow --preset tsan
else
	cmake --preset tsan
	cmake --build --preset tsan
	$(GPU_SANITIZER) --tool racecheck ctest --test-dir build/tsan --output-on-failure -L '$(LABEL)'
	$(GPU_SANITIZER) --tool synccheck ctest --test-dir build/tsan --output-on-failure -L '$(LABEL)'
endif

coverage: ## Coverage build, tests and report (build/coverage/report)
	cmake --workflow --preset coverage

format: ## Format the C++ and Python sources
	python3 -m checks.formatting

checks: __validators __format-check __lint ## Check without changing anything: validators, formatting, lint

# The validators shared with the git hooks: the file validators on every
# tracked file, the current branch name and the commit messages since develop.
__validators:
	python3 -m checks.conventions
	python3 -m checks.branch_name --current
	python3 -m checks.commit_message --since develop

__format-check:
	python3 -m checks.formatting --check

# clang-tidy, the clang-query rules, pylint, actionlint and the Python
# embedded in the workflows.
__lint:
	python3 -m checks.lint

clean: ## Remove the build trees
	rm -rf build

help: ## List the targets
	@awk -F ':.*## ' '/^[a-z-]+:.*## / { printf "  %-10s %s\n", $$1, $$2 }' \
		$(MAKEFILE_LIST)
