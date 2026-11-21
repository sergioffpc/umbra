# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Thin wrapper over the CMake presets (CMakePresets.json) for daily use and
# CI. Run inside the devcontainer.

.DEFAULT_GOAL := all
.PHONY: all test release asan tsan clean help

all: ## Debug build
	cmake --preset debug
	cmake --build --preset debug

test: all ## Debug build and run the tests
	ctest --preset debug

release: ## Release build (LTO) and run the tests
	cmake --workflow --preset release

asan: ## ASan + UBSan build and run the tests
	cmake --workflow --preset asan

tsan: ## TSan build and run the tests
	cmake --workflow --preset tsan

clean: ## Remove the build trees
	rm -rf build

help: ## List the targets
	@awk -F ':.*## ' '/^[a-z]+:.*## / { printf "  %-10s %s\n", $$1, $$2 }' \
		$(MAKEFILE_LIST)
