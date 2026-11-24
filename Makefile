# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Thin wrapper over the CMake presets (CMakePresets.json) for daily use and
# CI. Run inside the devcontainer.
#
# Targets prefixed with __ are the steps of a public target that CI runs as
# separate, parallel jobs; `make help` leaves them out.
#
# The programs it runs are variables, which the command line overrides, e.g.
# `make CMAKE=/opt/cmake/bin/cmake`.

SHELL = /bin/sh
CMAKE = cmake
CTEST = ctest
PYTHON = python3

# No implicit rules: every target is explicit.
.SUFFIXES:

.DEFAULT_GOAL := all
.PHONY: all check release sanitizers coverage format lint clean distclean \
	help __asan __tsan

all: ## Debug build
	$(CMAKE) --preset debug
	$(CMAKE) --build --preset debug

# LABEL=<regex> runs only the tests with a matching CTest label, e.g.
# `make check LABEL=gpu` on a GPU host.
check: all ## Debug build and run the tests (without the GPU tests; LABEL=gpu for only those)
ifeq ($(LABEL),)
	$(CTEST) --preset debug
else
	$(CTEST) --test-dir build/debug --output-on-failure -L '$(LABEL)'
endif

release: ## Release build (LTO) and run the tests
	$(CMAKE) --workflow --preset release

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
	$(CMAKE) --workflow --preset asan
else
	$(CMAKE) --preset asan
	$(CMAKE) --build --preset asan
	ASAN_OPTIONS=$(ASAN_GPU_OPTIONS) $(GPU_SANITIZER) --tool memcheck --leak-check full \
	  $(CTEST) --test-dir build/asan --output-on-failure -L '$(LABEL)'
	ASAN_OPTIONS=$(ASAN_GPU_OPTIONS) $(GPU_SANITIZER) --tool initcheck \
	  $(CTEST) --test-dir build/asan --output-on-failure -L '$(LABEL)'
endif

__tsan:
ifeq ($(LABEL),)
	$(CMAKE) --workflow --preset tsan
else
	$(CMAKE) --preset tsan
	$(CMAKE) --build --preset tsan
	$(GPU_SANITIZER) --tool racecheck $(CTEST) --test-dir build/tsan --output-on-failure -L '$(LABEL)'
	$(GPU_SANITIZER) --tool synccheck $(CTEST) --test-dir build/tsan --output-on-failure -L '$(LABEL)'
endif

coverage: ## Coverage build, tests and report (build/coverage/report)
	$(CMAKE) --workflow --preset coverage

format: ## Format the C++ and Python sources
	$(PYTHON) -m checks.formatting

# The validators shared with the git hooks (the file validators on every
# tracked file, the current branch name, the commit messages since develop),
# the formatting, then clang-tidy, the clang-query rules, pylint, actionlint
# and the Python embedded in the workflows.
lint: ## Check without changing anything: validators, formatting, static analysis
	$(PYTHON) -m checks.conventions
	$(PYTHON) -m checks.branch_name --current
	$(PYTHON) -m checks.commit_message --since develop
	$(PYTHON) -m checks.formatting --check
	$(PYTHON) -m checks.lint

# A build tree that was never configured has nothing to clean.
clean: ## Remove what the builds built, keeping their configuration
	@for tree in build/*/; do \
	  if [ -f "$${tree}CMakeCache.txt" ]; then \
	    $(CMAKE) --build "$$tree" --target clean || exit 1; \
	  fi; \
	done

distclean: ## Remove the build trees, with their configuration
	rm -rf build

help: ## List the targets
	@awk -F ':.*## ' '/^[a-z-]+:.*## / { printf "  %-10s %s\n", $$1, $$2 }' \
		$(MAKEFILE_LIST)
