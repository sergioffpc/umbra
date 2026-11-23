# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Thin wrapper over the CMake presets (CMakePresets.json) for daily use and
# CI. Run inside the devcontainer.

.DEFAULT_GOAL := all
.PHONY: all test release asan tsan coverage format lint checks clean help

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

asan: ## ASan + UBSan build and run the tests
	cmake --workflow --preset asan

tsan: ## TSan + UBSan integer checks build and run the tests
	cmake --workflow --preset tsan

coverage: ## Coverage build, tests and report (build/coverage/report)
	cmake --workflow --preset coverage

format: ## Format the C++ and Python sources
	python3 -m checks.formatting

lint: ## Run clang-tidy, clang-query rules, pylint, actionlint, embedded Python
	python3 -m checks.lint

checks: ## Run the validators shared with the git hooks and CI
	python3 -m checks.conventions
	python3 -m checks.branch_name --current
	python3 -m checks.commit_message --since develop

clean: ## Remove the build trees
	rm -rf build

help: ## List the targets
	@awk -F ':.*## ' '/^[a-z-]+:.*## / { printf "  %-10s %s\n", $$1, $$2 }' \
		$(MAKEFILE_LIST)
