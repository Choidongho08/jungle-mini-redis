# Mini Redis (CDH) build
#
#   make            라이브러리 + 테스트 바이너리 + 서버 빌드 (VARIANT=default)
#   make test       단위 + 기능 테스트 실행 및 요약
#   make utest      단위 테스트만 실행
#   make ftest      기능 테스트(실제 TCP 서버, 같은 스레드에서 구동)만 실행
#   make server     서버 바이너리 $(OUT)/mini-redis-server 빌드
#   make smoke      실제 바이너리를 띄워 bash /dev/tcp 로 PING/SET/GET·SIGTERM 종료 확인
#   make asan       ASan+UBSan 빌드로 단위+기능 테스트 실행 (누수 검사 포함)
#   make debug      -O0 -g 빌드 + 테스트
#   make release    -O2 -DNDEBUG 빌드 + 테스트
#   make clean      산출물 삭제
#
# 산출물은 repo 루트 build/ 아래 $(BUILD)/$(VARIANT)/ 에 생긴다.
# 모든 것이 싱글 스레드이므로 -pthread 를 쓰지 않는다.

.DEFAULT_GOAL := all

ROOT    := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
BUILD   ?= $(abspath $(ROOT)/../build/cdh)
VARIANT ?= default
OUT     := $(BUILD)/$(VARIANT)
OBJDIR  := $(OUT)/obj

CC      ?= cc
AR      ?= ar

WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wformat=2 -Werror
CPPFLAGS := -D_POSIX_C_SOURCE=200809L -I$(ROOT)/include
CFLAGS_STD := -std=c11 $(WARNINGS)
DEPFLAGS := -MMD -MP

ifeq ($(VARIANT),default)
  OPTFLAGS := -O2 -g
else ifeq ($(VARIANT),debug)
  OPTFLAGS := -O0 -g3
else ifeq ($(VARIANT),release)
  OPTFLAGS := -O2 -DNDEBUG
else ifeq ($(VARIANT),asan)
  OPTFLAGS := -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
              -fno-sanitize-recover=all -fno-pie
  # gcc 12 의 ASan 은 ASLR 엔트로피가 큰 커널(WSL2 6.x 등)에서 PIE 가 섀도 영역과
  # 겹치면 간헐적으로 DEADLYSIGNAL 무한 루프에 빠진다(컨테이너에서 실측 ~25%).
  # 비-PIE 로 링크하면 실행 파일이 낮은 고정 주소에 놓여 재현되지 않는다(30/30).
  LDFLAGS  += -fsanitize=address,undefined -no-pie
else
  $(error unknown VARIANT '$(VARIANT)' (default|debug|release|asan))
endif

COMMON_CFLAGS := $(CFLAGS_STD) $(OPTFLAGS) $(CFLAGS)

# 라이브러리: core + server. src/app/ 의 main 은 포함하지 않는다.
LIB_SRCS := $(sort $(wildcard $(ROOT)/src/core/*.c $(ROOT)/src/server/*.c))
LIB_OBJS := $(patsubst $(ROOT)/%.c,$(OBJDIR)/%.o,$(LIB_SRCS))
LIB      := $(OUT)/libminiredis.a

# 단위 테스트: tests/unit/*.c 각각이 독립 실행 파일.
TEST_SRCS    := $(sort $(wildcard $(ROOT)/tests/unit/*.c))
TEST_BINS    := $(patsubst $(ROOT)/tests/unit/%.c,$(OUT)/tests/%,$(TEST_SRCS))
TEST_CPPFLAGS := -I$(ROOT)/tests/support
# minitest.h 의 할당 실패 주입 훅
TEST_LDFLAGS := -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc

# 기능 테스트: tests/functional/*.c 각각이 독립 실행 파일.
FTEST_SRCS := $(sort $(wildcard $(ROOT)/tests/functional/*.c))
FTEST_BINS := $(patsubst $(ROOT)/tests/functional/%.c,$(OUT)/ftests/%,$(FTEST_SRCS))

# 서버 바이너리
SERVER_BIN := $(OUT)/mini-redis-server
APP_OBJS   := $(OBJDIR)/src/app/main.o

DEPS := $(LIB_OBJS:.o=.d) $(APP_OBJS:.o=.d) $(patsubst %,%.d,$(TEST_BINS) $(FTEST_BINS))

.PHONY: all lib tests ftests server test utest ftest smoke asan debug release clean help

all: lib tests ftests server

lib: $(LIB)

tests: $(TEST_BINS)

ftests: $(FTEST_BINS)

server: $(SERVER_BIN)

$(LIB): $(LIB_OBJS)
	@mkdir -p $(@D)
	$(AR) rcs $@ $^

$(OBJDIR)/%.o: $(ROOT)/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(COMMON_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(OUT)/tests/%: $(ROOT)/tests/unit/%.c $(LIB)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(TEST_CPPFLAGS) $(COMMON_CFLAGS) -MMD -MP -MF $@.d $< \
		-o $@ $(LDFLAGS) $(TEST_LDFLAGS) $(LIB)

$(OUT)/ftests/%: $(ROOT)/tests/functional/%.c $(LIB)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(TEST_CPPFLAGS) $(COMMON_CFLAGS) -MMD -MP -MF $@.d $< \
		-o $@ $(LDFLAGS) $(TEST_LDFLAGS) $(LIB)

$(SERVER_BIN): $(APP_OBJS) $(LIB)
	@mkdir -p $(@D)
	$(CC) $(COMMON_CFLAGS) $(APP_OBJS) -o $@ $(LDFLAGS) $(LIB)

# 각 테스트를 모두 실행한 뒤(중간 실패로 멈추지 않음) 합계를 출력하고 실패 시 비0 종료.
# $(1)=종류 이름, $(2)=바이너리 목록
define run_suites
	@pass=0; fail=0; failed=""; \
	for t in $(2); do \
		echo "== $$(basename $$t) [$(VARIANT)]"; \
		if $$t; then pass=$$((pass+1)); else fail=$$((fail+1)); failed="$$failed $$(basename $$t)"; fi; \
	done; \
	echo "== $(1) suites [$(VARIANT)]: $$pass passed, $$fail failed$$failed"; \
	[ $$fail -eq 0 ]
endef

utest: $(TEST_BINS)
	$(call run_suites,unit,$(TEST_BINS))

ftest: $(FTEST_BINS)
	$(call run_suites,functional,$(FTEST_BINS))

# unit 이 실패해도 functional 까지 돌려 전체 그림을 보여 준 뒤 실패로 끝낸다.
test: $(TEST_BINS) $(FTEST_BINS)
	@rc=0; \
	$(MAKE) --no-print-directory -f $(ROOT)/Makefile utest || rc=1; \
	$(MAKE) --no-print-directory -f $(ROOT)/Makefile ftest || rc=1; \
	exit $$rc

smoke: $(SERVER_BIN)
	@bash $(ROOT)/tests/functional/smoke.sh $(SERVER_BIN)

asan:
	@ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1 \
		$(MAKE) --no-print-directory -f $(ROOT)/Makefile VARIANT=asan test

debug:
	@$(MAKE) --no-print-directory -f $(ROOT)/Makefile VARIANT=debug test

release:
	@$(MAKE) --no-print-directory -f $(ROOT)/Makefile VARIANT=release test

clean:
	rm -rf $(BUILD)

help:
	@echo "targets: all lib tests ftests server test utest ftest smoke asan debug release clean help"
	@echo "vars:    VARIANT=default|debug|release|asan  BUILD=$(BUILD)"

-include $(DEPS)
-include $(ROOT)/bench/bench.mk
