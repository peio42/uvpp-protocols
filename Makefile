.PHONY: configure build test examples clean

BUILD_DIR ?= build
CMAKE ?= cmake

configure:
	$(CMAKE) -S . -B $(BUILD_DIR)

build: configure
	$(CMAKE) --build $(BUILD_DIR)

test: build
	$(CMAKE) --build $(BUILD_DIR) --target test

examples: configure
	$(CMAKE) --build $(BUILD_DIR) --target uvpp_protocols_http_server_example

clean:
	$(CMAKE) -E rm -rf $(BUILD_DIR)

