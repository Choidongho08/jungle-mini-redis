CC ?= cc
PYTHON ?= python3
BUILD_DIR ?= build
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Isrc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O2

SERVER_SOURCES = src/main.c src/server/server.c src/server/client.c src/commands/command.c src/common/net.c src/common/time_utils.c src/protocol/resp.c src/persistence/persistence.c
SERVER_HEADERS = $(wildcard src/*/*.h)

SERVER = $(BUILD_DIR)/mini-redis
UNIT_TEST = $(BUILD_DIR)/test_store
REHASH_TEST = $(BUILD_DIR)/test_rehash
CLIENT_TEST = $(BUILD_DIR)/test_client
LOOKUP_BENCH = $(BUILD_DIR)/bench_lookup
ARRAY_SERVERS = $(BUILD_DIR)/mini-redis-linear $(BUILD_DIR)/mini-redis-sorted

.PHONY: all test bench-lookup bench-mixed clean

all: $(SERVER)

$(SERVER): $(SERVER_SOURCES) src/storage/store.c $(SERVER_HEADERS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SERVER_SOURCES) src/storage/store.c $(LDLIBS)

$(UNIT_TEST): tests/test_store.c src/storage/store.c src/storage/store.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_store.c src/storage/store.c $(LDLIBS)

$(REHASH_TEST): tests/test_rehash.c src/storage/store.c src/storage/store.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_rehash.c $(LDLIBS)

$(CLIENT_TEST): tests/test_client.c $(SERVER_SOURCES) src/storage/store.c $(SERVER_HEADERS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_client.c $(filter-out src/main.c src/server/server.c,$(SERVER_SOURCES)) src/storage/store.c $(LDLIBS)

$(LOOKUP_BENCH): tests/bench_lookup.c src/storage/store.c src/storage/store.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/bench_lookup.c $(LDLIBS)

$(BUILD_DIR)/mini-redis-linear: SORTED_BUCKETS = 0
$(BUILD_DIR)/mini-redis-sorted: SORTED_BUCKETS = 1
$(ARRAY_SERVERS): $(SERVER_SOURCES) tests/array_store.c $(SERVER_HEADERS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -DSORTED_BUCKETS=$(SORTED_BUCKETS) -o $@ $(SERVER_SOURCES) tests/array_store.c $(LDLIBS)

$(BUILD_DIR):
	mkdir -p $@

test: $(SERVER) $(UNIT_TEST) $(REHASH_TEST) $(CLIENT_TEST) $(LOOKUP_BENCH)
	$(UNIT_TEST)
	$(REHASH_TEST)
	$(CLIENT_TEST)
	$(LOOKUP_BENCH) --check
	MINI_REDIS_BIN=$(SERVER) PYTHONDONTWRITEBYTECODE=1 $(PYTHON) -m unittest discover -s tests -v

bench-lookup: $(LOOKUP_BENCH)
	$(LOOKUP_BENCH)

bench-mixed: $(SERVER) $(ARRAY_SERVERS)
	PYTHONDONTWRITEBYTECODE=1 $(PYTHON) -m examples.bench_mixed $(SERVER) $(ARRAY_SERVERS)

clean:
	rm -f $(SERVER) $(UNIT_TEST) $(REHASH_TEST) $(CLIENT_TEST) $(LOOKUP_BENCH) $(ARRAY_SERVERS)
