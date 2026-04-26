CC = gcc

ifeq ($(DEBUG),y)
CFLAGS += -D__DEBUG__ -O0 -g -ggdb
else
CFLAGS += -O3
endif

SRCS := dpdk.h
APP_DIR := app

PKGCONF ?= pkg-config
PC_FILE := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(PKGCONF) --path libdpdk 2>/dev/null)
CFLAGS += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(PKGCONF) --cflags libdpdk)

LDFLAGS_SHARED = $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) $(PKGCONF) --libs libdpdk)

CFLAGS += -DALLOW_EXPERIMENTAL_API -lm -lstdc++

# dpdk_echo: $(SRCS) Makefile $(PC_FILE)
# 	$(CC) $(CFLAGS) $(SRCS) -o $@ $(LDFLAGS) $(LDFLAGS_SHARED)
all: packet_gen_client echo_server echo_server_checksum_exp

packet_gen_client: $(APP_DIR)/packet_gen_client.c $(APP_DIR)/dpdk.h Makefile $(PC_FILE)
	$(CC) $(CFLAGS) $(APP_DIR)/packet_gen_client.c -o $@ $(LDFLAGS) $(LDFLAGS_SHARED)

echo_server: $(APP_DIR)/echo_server.c $(APP_DIR)/dpdk.h Makefile $(PC_FILE)
	$(CC) $(CFLAGS) $(APP_DIR)/echo_server.c -o $@ $(LDFLAGS) $(LDFLAGS_SHARED)

echo_server_checksum_exp: $(APP_DIR)/echo_server_checksum_exp.c $(APP_DIR)/dpdk.h Makefile $(PC_FILE)
	$(CC) $(CFLAGS) $(APP_DIR)/echo_server_checksum_exp.c -o $@ $(LDFLAGS) $(LDFLAGS_SHARED)

clean:
	rm -f packet_gen_client echo_server echo_server_checksum_exp
