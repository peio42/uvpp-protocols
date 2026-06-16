.PHONY: configure build test examples build-gcc build-clang build-all test-gcc test-clang test-all package clean

CXX ?= g++
CC ?= cc
CXX_ID ?= $(notdir $(CXX))
BUILD_DIR ?= build
DIST_DIR ?= dist
CMAKE ?= cmake
CONFIGURE_ARGS ?=

configure:
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_C_COMPILER=$(CC) -DCMAKE_CXX_COMPILER=$(CXX) $(CONFIGURE_ARGS)

build: configure
	$(CMAKE) --build $(BUILD_DIR)

test: build
	$(CMAKE) --build $(BUILD_DIR) --target test

examples: configure
	$(CMAKE) --build $(BUILD_DIR) --target uvpp_protocols_http_server_example

build-gcc:
	$(MAKE) build CC=gcc CXX=g++ CXX_ID=gcc BUILD_DIR=build/gcc

build-clang:
	$(MAKE) build CC=clang CXX=clang++ CXX_ID=clang BUILD_DIR=build/clang

build-all: build-gcc build-clang

test-gcc:
	$(MAKE) test CC=gcc CXX=g++ CXX_ID=gcc BUILD_DIR=build/gcc

test-clang:
	$(MAKE) test CC=clang CXX=clang++ CXX_ID=clang BUILD_DIR=build/clang

test-all: test-gcc test-clang

package:
	@if [ -z "$(VERSION)" ]; then \
		echo "VERSION is required, example: make package VERSION=0.1.0"; \
		exit 1; \
	fi
	@if ! printf '%s\n' "$(VERSION)" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+([-.][0-9A-Za-z.-]+)?$$'; then \
		echo "Invalid VERSION: $(VERSION)"; \
		echo "Expected semver-like value, example: 0.1.0 or 0.1.0-rc.1"; \
		exit 1; \
	fi
	printf '%s\n' "$(VERSION)" > VERSION
	rm -rf $(DIST_DIR)/uvpp-protocols-$(VERSION) $(DIST_DIR)/uvpp-protocols-$(VERSION).tar.gz $(DIST_DIR)/checksums.txt
	mkdir -p $(DIST_DIR)/uvpp-protocols-$(VERSION)
	cp -R README.md CMakeLists.txt Makefile cmake VERSION include src docs examples tests $(DIST_DIR)/uvpp-protocols-$(VERSION)/
	@if [ -f LICENSE ]; then cp LICENSE $(DIST_DIR)/uvpp-protocols-$(VERSION)/; fi
	tar -czf $(DIST_DIR)/uvpp-protocols-$(VERSION).tar.gz -C $(DIST_DIR) uvpp-protocols-$(VERSION)
	cd $(DIST_DIR) && sha256sum uvpp-protocols-$(VERSION).tar.gz > checksums.txt

clean:
	$(CMAKE) -E rm -rf build $(DIST_DIR)
