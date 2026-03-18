CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
INCS := -Iinclude

all: bin/wifi-daemon

bin/wifi-daemon: src/wifi_daemon.c include/proto/wifi_proto.h
	@mkdir -p bin
	$(CC) $(CFLAGS) $(INCS) -o $@ src/wifi_daemon.c

unit-test: tests/unit/test_proto.c
	$(CC) $(CFLAGS) $(INCS) -o /tmp/test_proto tests/unit/test_proto.c -lcmocka
	/tmp/test_proto

integration-test: bin/wifi-daemon
	bash tests/integration/test_ipc.sh

clean:
	rm -rf bin /tmp/test_proto
