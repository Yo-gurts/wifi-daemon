TOOLCHAIN_DIR ?= ../host-tools/gcc/arm-gnu-toolchain-11.3.rel1-x86_64-arm-none-linux-musleabihf/bin
CROSS_COMPILE ?= $(TOOLCHAIN_DIR)/arm-none-linux-musleabihf-
CC := $(CROSS_COMPILE)gcc
STRIP ?= $(CROSS_COMPILE)strip

CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
INCS := -Iinclude
SRCS := src/wifi_daemon.c src/wpa_ctrl_compat.c

all: bin/wifi-daemon

bin/wifi-daemon: $(SRCS) include/proto/wifi_proto.h include/third_party/wpa_ctrl.h
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INCS) -o $@ $(SRCS)

strip: bin/wifi-daemon
	$(STRIP) $<

unit-test: tests/unit/test_proto.c
	$(CC) $(CFLAGS) $(INCS) -o /tmp/test_proto tests/unit/test_proto.c -lcmocka
	/tmp/test_proto

integration-test: bin/wifi-daemon
	bash tests/integration/test_ipc.sh

clean:
	rm -rf bin /tmp/test_proto
