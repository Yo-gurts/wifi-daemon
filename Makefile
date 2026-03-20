# Makefile for wifi-daemon
# Default target: ARM cross-compilation

CONFIG ?= ARM
CMAKE_BUILD_TYPE ?= Debug

TOOLCHAIN_FILE :=
BUILD_DIR :=
ifeq ($(CONFIG), ARM)
	TOOLCHAIN_FILE = toolchains/arm-none-linux-musleabihf.cmake
	BUILD_DIR = build/arm-none-linux-musleabihf
else ifeq ($(CONFIG), ARM64)
	TOOLCHAIN_FILE = toolchains/aarch64-linux-gnu.cmake
	BUILD_DIR = build/aarch64-linux-gnu
else ifeq ($(CONFIG), X86)
	BUILD_DIR = build/x86_64
endif

TOOLCHAIN_ARG = $(if $(TOOLCHAIN_FILE),-DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN_FILE),)
TOOLCHAIN_ROOT_ARG = $(if $(and $(TOOLCHAIN_FILE),$(TOOLCHAIN_ROOT_DIR)),-DTOOLCHAIN_ROOT_DIR=$(TOOLCHAIN_ROOT_DIR),)

.PHONY: all clean help

all:
	cmake -B $(BUILD_DIR) $(TOOLCHAIN_ARG) $(TOOLCHAIN_ROOT_ARG) -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) $(CURDIR)
	cmake --build $(BUILD_DIR) -j

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "Usage: make [CONFIG=ARM|ARM64|X86] [TOOLCHAIN_ROOT_DIR=/path/to/toolchain]"
	@echo ""
	@echo "Targets:"
	@echo "  all        Build the project (default: ARM)"
	@echo "  clean      Clean build files"
	@echo ""
	@echo "Examples:"
	@echo "  make CONFIG=ARM                                          # use arm-none toolchain from PATH"
	@echo "  make CONFIG=ARM TOOLCHAIN_ROOT_DIR=/path/to/toolchain   # ARM cross-compilation"
	@echo "  make CONFIG=ARM64 TOOLCHAIN_ROOT_DIR=/path/to/toolchain # ARM64 cross-compilation"
	@echo "  make CONFIG=X86                                   # x86_64 native build"
