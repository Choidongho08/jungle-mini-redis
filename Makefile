# Mini Redis build
#
#   make                  build/mini-redis
#   make test-unit        tests/test_*.c 를 각각 빌드해서 실행
#   make test-integration tests/integration.sh 실행
#   make test             test-unit + test-integration
#   make asan             AddressSanitizer/UBSan 빌드로 make test 실행
#   make clean
#
# 새 소스(src/*.c)나 단위 테스트(tests/test_*.c)는 이 파일을 고치지 않아도
# 와일드카드로 자동 포함된다.

CC          ?= gcc
CFLAGS_BASE := -std=c11 -D_GNU_SOURCE -Wall -Wextra -g -Isrc
OPTFLAGS    ?= -O2
CFLAGS      ?= $(CFLAGS_BASE) $(OPTFLAGS)
LDFLAGS     ?=

BUILD       ?= build

SRCS        := $(wildcard src/*.c)
HDRS        := $(wildcard src/*.h)
LIB_SRCS    := $(filter-out src/main.c,$(SRCS))
TEST_SRCS   := $(wildcard tests/test_*.c)
TESTS       := $(patsubst tests/%.c,$(BUILD)/%,$(TEST_SRCS))
SERVER      := $(BUILD)/mini-redis

ASAN_FLAGS  := -fsanitize=address,undefined -fno-omit-frame-pointer
# GCC 12 의 ASan 은 ASLR 엔트로피가 큰 최신 커널(WSL2 6.x 등)에서 시작하자마자 죽는다.
# 그런 환경이면 누수 검사(LSan)와 UBSan 만으로 대체한다.
LSAN_FLAGS  := -fsanitize=leak,undefined -fno-omit-frame-pointer

.PHONY: all test test-unit test-integration asan clean

all: $(SERVER)

$(BUILD):
	mkdir -p $@

$(SERVER): $(SRCS) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

# 일반 단위 테스트: 테스트 파일 + main.c 를 뺀 모든 소스
$(BUILD)/test_%: tests/test_%.c $(LIB_SRCS) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(LIB_SRCS) $(LDFLAGS)

# SipHash 테스트는 공식 테스트 벡터가 있는 SipHash-2-4 로 따로 컴파일한다.
# (서버는 Redis 와 같은 SipHash-1-2 를 쓴다. design TD-003)
$(BUILD)/test_siphash: tests/test_siphash.c src/siphash.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -DSIPHASH_C_ROUNDS=2 -DSIPHASH_D_ROUNDS=4 -o $@ tests/test_siphash.c src/siphash.c $(LDFLAGS)

# RUN: 테스트 실행 래퍼 (make asan 에서 tests/run_sanitized.sh 로 지정)
RUN ?=

test-unit: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; $(RUN) $$t; done

test-integration: $(SERVER)
	@if [ -f tests/integration.sh ]; then \
		SERVER_BIN=$(SERVER) bash tests/integration.sh; \
	else \
		echo "tests/integration.sh 가 아직 없습니다 (T3 에서 추가)"; \
	fi

test: test-unit test-integration

asan:
	@mkdir -p build
	@echo 'int main(void){return 0;}' > build/.asan_probe.c
	@if $(CC) $(ASAN_FLAGS) -o build/.asan_probe build/.asan_probe.c 2>/dev/null \
				&& bash tests/run_sanitized.sh ./build/.asan_probe >/dev/null 2>&1; then \
		echo "== sanitizer: address,undefined"; \
		$(MAKE) BUILD=build/asan CFLAGS="$(CFLAGS_BASE) -O1 $(ASAN_FLAGS)" LDFLAGS="$(ASAN_FLAGS)" RUN="bash tests/run_sanitized.sh" test; \
	else \
		echo "== WARNING: ASan 을 이 환경에서 실행할 수 없어 leak,undefined 로 대체합니다"; \
		$(MAKE) BUILD=build/lsan CFLAGS="$(CFLAGS_BASE) -O1 $(LSAN_FLAGS)" LDFLAGS="$(LSAN_FLAGS)" RUN="bash tests/run_sanitized.sh" test; \
	fi

clean:
	rm -rf build
