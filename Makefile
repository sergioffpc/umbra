# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# A convenience for developers over the CMake presets (CMakePresets.json) and
# the shared checks. Run inside the devcontainer. CI does not use it: it runs
# the presets and the checks directly.
#
# The programs it runs are variables, which the command line overrides, e.g.
# `make CMAKE=/opt/cmake/bin/cmake`.

SHELL = /bin/sh
CMAKE = cmake
CTEST = ctest
PYTHON = python3

# No implicit rules: every target is explicit.
.SUFFIXES:

# The CMake preset that `make all` and `make check` build: debug (the
# default) or release (LTO), e.g. `make check PRESET=release`.
PRESET = debug
ifeq ($(filter $(PRESET),debug release),)
$(error PRESET must be debug or release, not '$(PRESET)')
endif

.DEFAULT_GOAL := all
.PHONY: all check check-asan check-tsan check-all coverage format lint clean \
	distclean help

all: ## Build (PRESET=debug, the default, or PRESET=release)
	$(CMAKE) --preset $(PRESET)
	$(CMAKE) --build --preset $(PRESET)

# LABEL=<regex> runs only the tests with a matching CTest label, e.g.
# `make check LABEL=gpu` on a GPU host.
check: all ## Build and run the tests (without the GPU tests; LABEL=gpu for only those)
ifeq ($(LABEL),)
	$(CTEST) --preset $(PRESET)
else
	$(CTEST) --test-dir build/$(PRESET) --output-on-failure -L '$(LABEL)'
endif

# LABEL=<regex> also selects the tests of the sanitizer builds, and runs them
# under NVIDIA's compute-sanitizer too, which checks the kernels they launch:
# memcheck (out-of-bounds and misaligned device accesses, device memory
# leaks) and initcheck (reads of uninitialized device memory) with asan,
# racecheck (shared-memory races) and synccheck (misused barriers) with tsan.
# For example `make check-asan LABEL=gpu` on a GPU host.
#
# AddressSanitizer keeps the shadow gap of the address space unmapped, where
# the CUDA driver maps device memory, so that cudaMalloc would fail with out
# of memory: protect_shadow_gap=0 lets the driver map it.
GPU_SANITIZER = compute-sanitizer --error-exitcode 1 --target-processes all
ASAN_GPU_OPTIONS = protect_shadow_gap=0

check-asan: ## ASan + UBSan build and tests (LABEL=gpu: the GPU tests, also under compute-sanitizer)
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

check-tsan: ## TSan + UBSan integer checks build and tests (LABEL=gpu: the GPU tests, also under compute-sanitizer)
ifeq ($(LABEL),)
	$(CMAKE) --workflow --preset tsan
else
	$(CMAKE) --preset tsan
	$(CMAKE) --build --preset tsan
	$(GPU_SANITIZER) --tool racecheck $(CTEST) --test-dir build/tsan --output-on-failure -L '$(LABEL)'
	$(GPU_SANITIZER) --tool synccheck $(CTEST) --test-dir build/tsan --output-on-failure -L '$(LABEL)'
endif

check-all: lint check check-asan check-tsan ## lint, check, check-asan and check-tsan: everything before a pull request

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
	@awk -F ':.*## ' '/^[a-z-]+:.*## / { printf "  %-11s %s\n", $$1, $$2 }' \
		$(MAKEFILE_LIST)
