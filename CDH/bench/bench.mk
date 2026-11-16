# Mini Redis 벤치마크 조각. CDH/Makefile 이 `-include bench/bench.mk` 로 끌어 쓴다.
# (아직 include 가 없으면:  make -C CDH -f Makefile -f bench/bench.mk bench)
#
# Makefile 의 ROOT/BUILD/CC/AR/CPPFLAGS/WARNINGS 를 재사용한다.
# 벤치는 항상 release(-O2 -DNDEBUG) 코어 라이브러리에 링크해야 숫자가 의미 있으므로
# VARIANT 와 무관하게 $(BUILD)/release 의 라이브러리를 재귀 make 로 만든다.

BENCH_OUT  := $(BUILD)/bench
BENCH_LIB  := $(BUILD)/release/libminiredis.a
BENCH_CFLAGS := $(CFLAGS_STD) -O2 -DNDEBUG $(CFLAGS)
BENCH_BINS := $(BENCH_OUT)/bench_core $(BENCH_OUT)/bench_cache
# 추가 인자 예: make bench-core BENCH_CORE_ARGS="--quick"
BENCH_CORE_ARGS ?=
BENCH_CACHE_ARGS ?=

.PHONY: bench bench-core bench-cache bench-build

# 라이브러리는 항상 재귀 make 에 위임(자체 의존성 추적). FORCE 로 매번 확인한다.
# VARIANT=release 일 때는 BENCH_LIB 가 Makefile 의 $(LIB) 와 같은 파일이므로 이 규칙을
# 정의하지 않는다. 정의하면 재귀 make 가 다시 자기 자신을 부르며 끝없이 프로세스를 만든다.
ifneq ($(VARIANT),release)
$(BENCH_LIB): FORCE
	@$(MAKE) --no-print-directory -f $(ROOT)/Makefile VARIANT=release lib
endif

FORCE:

$(BENCH_OUT)/%: $(ROOT)/bench/%.c $(ROOT)/bench/bench_common.h $(BENCH_LIB)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(BENCH_CFLAGS) $< -o $@ $(BENCH_LIB) -lm

bench-build: $(BENCH_BINS)

bench-core: $(BENCH_OUT)/bench_core
	$(BENCH_OUT)/bench_core $(BENCH_CORE_ARGS)

bench-cache: $(BENCH_OUT)/bench_cache
	$(BENCH_OUT)/bench_cache $(BENCH_CACHE_ARGS)

# 두 벤치는 한 레시피 안에서 차례로 실행한다(동시에 돌면 서로의 측정을 흔든다).
bench: bench-build
	$(BENCH_OUT)/bench_core $(BENCH_CORE_ARGS)
	$(BENCH_OUT)/bench_cache $(BENCH_CACHE_ARGS)
