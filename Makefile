# trade-ngin developer entrypoints.
#
# Fresh clone, first time:
#     sudo make deps      # system libraries -- needs root, one time only
#     make install        # configure + compile
#     make test           # run the suite
#
# Overridable on the command line, e.g.  make install BUILD_TYPE=Debug
SHELL := /bin/bash

BUILD_DIR  ?= build
BUILD_TYPE ?= Release
JOBS       ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# NLopt installs its CMake config in a distro-specific place. This is the path
# used by the CI image; it is only passed to CMake if it actually exists, so
# systems that put it elsewhere (or expose it on the default search path) still
# configure cleanly. Override with:  make install NLOPT_DIR=/usr/local/lib/cmake/nlopt
NLOPT_DIR ?= /usr/lib/x86_64-linux-gnu/cmake/nlopt

.DEFAULT_GOAL := help
.PHONY: help deps configure build install test clean rebuild

help: ## Show available targets
	@echo "trade-ngin -- C++ execution engine"
	@echo ""
	@echo "First time on a fresh machine:  sudo make deps && make install"
	@echo ""
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | sort | \
		awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-12s\033[0m %s\n", $$1, $$2}'

deps: ## Install system libraries (needs root: sudo make deps)
	@if [ "$$(uname)" = "Darwin" ]; then \
		echo "==> macOS detected"; \
		bash requirements/install_macos.sh; \
	else \
		echo "==> Linux detected"; \
		bash requirements/install_ubuntu.sh; \
	fi

configure: ## Run the CMake configure step
	@cmake -S . -B $(BUILD_DIR) \
		-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
		$(if $(wildcard $(NLOPT_DIR)),-DNLopt_DIR=$(NLOPT_DIR),)

build: configure ## Compile the project
	@cmake --build $(BUILD_DIR) -j$(JOBS)

install: build ## Configure and compile (main entrypoint)
	@echo ""
	@echo "Built successfully -> $(BUILD_DIR)/bin/$(BUILD_TYPE)"
	@echo "Run the tests with: make test"

test: build ## Run the ctest suite
	@cd $(BUILD_DIR) && ctest --output-on-failure

clean: ## Remove build artifacts
	@rm -rf $(BUILD_DIR)
	@echo "Removed $(BUILD_DIR)/"

rebuild: clean install ## Clean, then build from scratch
