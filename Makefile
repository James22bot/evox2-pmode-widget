CXX ?= g++
ZIG ?= zig

HOST_FLAGS := -std=c++20 -Wall -Wextra -Wpedantic -Werror -Iinclude
HOST_SANITIZERS := -fsanitize=address,undefined
WINDOWS_FLAGS := -target x86_64-windows-gnu -std=c++20 -O2 -s -Wall -Wextra -Wpedantic -Werror -Wno-nullability-completeness -fstack-protector-strong -DUNICODE -D_UNICODE -Iinclude
MODULE_SHA256 := c38fd116e7aff4d1fdb0a494e296be0a6708e5a22fc72f14587442fb7f8f7906
MODULE_PATH := third_party/PawnIO.Modules-0.1.6/LpcACPIEC.bin

.PHONY: all test verify-module windows clean

all: test windows

test: build/test_core build/test_overlay_model
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./build/test_core
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./build/test_overlay_model

build/test_core: include/evox2/core.hpp src/core.cpp tests/test_core.cpp
	mkdir -p build
	$(CXX) $(HOST_FLAGS) $(HOST_SANITIZERS) src/core.cpp tests/test_core.cpp -o $@

build/test_overlay_model: include/evox2/core.hpp include/evox2/overlay_model.hpp src/core.cpp src/overlay_model.cpp tests/test_overlay_model.cpp
	mkdir -p build
	$(CXX) $(HOST_FLAGS) $(HOST_SANITIZERS) src/core.cpp src/overlay_model.cpp tests/test_overlay_model.cpp -o $@

verify-module:
	printf '%s  %s\n' '$(MODULE_SHA256)' '$(MODULE_PATH)' | sha256sum -c -

windows: build/windows/evox2-pmode-overlay.exe

build/windows/app_resources.o: resources/app.rc resources/app.manifest
	mkdir -p build/windows
	$(ZIG) rc /nologo /c 65001 /:auto-includes gnu /:output-format coff /:target x86_64 /i resources /fo $@ resources/app.rc

build/windows/evox2-pmode-overlay.exe: verify-module include/evox2/core.hpp include/evox2/overlay_model.hpp include/evox2/windows_ec_backend.hpp src/core.cpp src/overlay_model.cpp src/windows_ec_backend.cpp src/overlay_main.cpp build/windows/app_resources.o
	mkdir -p build/windows
	$(ZIG) c++ $(WINDOWS_FLAGS) src/core.cpp src/overlay_model.cpp src/windows_ec_backend.cpp src/overlay_main.cpp build/windows/app_resources.o -lbcrypt -ladvapi32 -lshell32 -lgdi32 -luser32 -Wl,--subsystem,windows -o $@

clean:
	rm -rf build
