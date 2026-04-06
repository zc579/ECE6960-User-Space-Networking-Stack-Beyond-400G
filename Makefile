TARGETS = echo_server
APP_DIR = app

TARGETS += packet_gen_client

CFLAGS += -O3 -Wall -Wextra -Wno-unused-parameter
CFLAGS += $(shell pkg-config --cflags libdpdk)
LDFLAGS += $(shell pkg-config --libs libdpdk)

.PHONY: all clean

all: $(TARGETS)

echo_server: $(APP_DIR)/echo_server.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

packet_gen_client: $(APP_DIR)/packet_gen_client.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGETS)
