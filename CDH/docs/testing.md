# 테스트

## 전략

| 층 | 대상 | 방식 | 위치 |
|---|---|---|---|
| 단위 | ht, store, aof, glob, resp, command | 라이브러리에 직접 링크, 네트워크 없음. 가짜 시계(`mr_clock`) 주입으로 TTL을 결정적으로 제어 | `tests/unit/test_*.c` |
| 기능 | 서버 전체(실제 TCP 소켓) | `mr_server_run_once()`로 서버 한 틱과 클라이언트 소켓 조작을 **같은 스레드에서 번갈아** 수행(스레드·sleep 없이 결정적) | `tests/functional/test_server.c` |
| 스모크 | 실제 `mini-redis-server` 바이너리 | bash `/dev/tcp`로 RESP 송수신, 시그널 종료 코드, 재시작 복구 | `tests/functional/smoke.sh` |
| 안전성 | 전체 | `make asan`: ASan+UBSan(누수 검사) 빌드로 단위+기능 재실행 | `Makefile` |

- 프레임워크: 직접 만든 `tests/support/minitest.h`(`TEST`, `CHECK`, `ASSERT`, `MT_MAIN`). `CHECK`는 실패를 기록하고 계속, `ASSERT`는 해당 테스트를 끝낸다.
- **할당 실패 주입**: 테스트 바이너리는 `-Wl,--wrap=malloc,calloc,realloc`으로 링크되어 `mt_fail_alloc_after(n)`이 n번째 할당을 NULL로 만든다. 라이브러리 코드를 수정하지 않고 OOM 경로를 시험한다(`test_alloc_failure_injection`, `test_writer_nomem`, `test_parse_nomem`, `test_reply_nomem`).
- 빌드 경고는 `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wformat=2 -Werror`다.

## 실행 방법

```sh
docker compose run --rm dev make -C CDH test    # 단위 + 기능
docker compose run --rm dev make -C CDH asan    # ASan/UBSan
docker compose run --rm dev make -C CDH smoke   # 실제 바이너리
```

호스트(Windows)에는 gcc가 없으므로 모두 컨테이너에서 실행한다.

## 실제 실행 결과

컨테이너(Debian 12, gcc 12)에서 이 문서 작성 시 실행한 결과다. 숫자는 출력 그대로이며 "checks"는 `CHECK`/`ASSERT` 평가 횟수다.

`make -C CDH test` (VARIANT=default), `make -C CDH asan` 모두 동일하게 전부 통과:

| 스위트 | 케이스 | 통과 | 실패 | checks |
|---|---:|---:|---:|---:|
| test_aof | 10 | 10 | 0 | 5,170 |
| test_command | 16 | 16 | 0 | 264 |
| test_glob | 7 | 7 | 0 | 59 |
| test_ht | 13 | 13 | 0 | 352,004 |
| test_resp | 16 | 16 | 0 | 112,971 |
| test_store | 14 | 14 | 0 | 28,888 |
| **단위 합계** | **76** | **76** | **0** | |
| test_server (기능) | 17 | 17 | 0 | 2,857 |

- 요약 줄: `== unit suites [default]: 6 passed, 0 failed`, `== functional suites [default]: 1 passed, 0 failed`(asan도 `[asan]`로 동일).
- `make -C CDH smoke`: `[ OK ]` 6항목(CLI 인자, PING/SET/GET/INCR/QUIT, 사용 중 포트 오류, SIGTERM→0, AOF 재기동 복구, SIGINT→0) 후 `smoke: all checks passed`.
- `debug`/`release` 변형은 이번에 실행하지 않았다(미확인).

## 케이스 목록

### 단위: `test_ht.c` (해시 테이블)

`test_basic_ops`, `test_initial_capacity_rounds_to_pow2`, `test_special_keys`, `test_full_collision_chain_integrity`, `test_ops_during_incremental_rehash`, `test_shrink`, `test_large_volume`, `test_value_destructor_counts`, `test_scan_returns_all_across_growth`, `test_scan_returns_all_across_shrink`, `test_scan_without_mutation_is_exact`, `test_scan_delete_action`, `test_alloc_failure_injection`

### 단위: `test_store.c`

`test_set_get_del_exists`, `test_nx_xx`, `test_px_expiry_boundary_and_lazy`, `test_set_ttl_edge_values`, `test_expire_ttl_persist`, `test_incr_family`, `test_keys_and_scan`, `test_invalidate_and_flushall`, `test_stats`, `test_sweep_samples_and_progress`, `test_sweep_time_budget`, `test_loading_mode_preserves_past_ttl`, `test_large_value`, `test_alloc_failure_injection`

### 단위: `test_aof.c`

`test_roundtrip_all_commands`, `test_binary_values_with_nul_and_newlines`, `test_expired_records_are_dropped`, `test_truncated_tail_is_cut_at_every_offset`, `test_middle_corruption_is_reported`, `test_rewrite_compacts_and_survives_restart`, `test_io_error_paths`, `test_write_failure_rolls_back_partial_record`, `test_large_value_roundtrip`, `test_alloc_failure_injection`

### 단위: `test_resp.c`, `test_glob.c`, `test_command.c`

- resp: `test_writers`, `test_writer_nomem`, `test_parse_i64`, `test_parse_multibulk`, `test_parse_binary_and_empty_bulk`, `test_parse_byte_by_byte`, `test_parse_pipeline`, `test_parse_inline`, `test_parse_empty_multibulk`, `test_parse_no_prealloc_for_declared_count`, `test_parse_buffer_moves_between_calls`, `test_parse_errors`, `test_parse_error_is_sticky`, `test_parse_nomem`, `test_fuzz_split_streams`, `test_fuzz_garbage_and_mutations`
- glob: `test_literal_and_empty`, `test_star`, `test_question`, `test_class`, `test_escape`, `test_binary_safe`, `test_pathological_patterns_are_fast`
- command: `test_ping_echo_select_quit`, `test_unknown_and_empty`, `test_set_get_options`, `test_set_binary_and_utf8`, `test_set_expire_options`, `test_expire_family`, `test_incr_family`, `test_del_exists_dbsize_flushall`, `test_keys_pattern`, `test_scan_full_iteration`, `test_invalidate`, `test_info_command_config`, `test_info_without_server_info`, `test_reply_nomem`, `test_aof_roundtrip_all_writes`, `test_aof_failure_blocks_writes`

### 기능: `test_server.c` (실제 TCP)

`test_all_commands_over_tcp`, `test_split_one_byte_at_a_time`, `test_pipeline_100`, `test_inline_commands`, `test_large_binary_utf8_values`, `test_ttl_with_fake_clock`, `test_scan_consistency`, `test_100_clients_incr`, `test_maxclients`, `test_protocol_error_closes_connection`, `test_abrupt_disconnects`, `test_output_buffer_limit`, `test_idle_timeout`, `test_quit_with_pipelined_tail`, `test_run_stop_and_port_in_use`, `test_aof_restart_and_rewrite`, `test_aof_corrupt_refuses_start`

## 엣지 케이스

| 영역 | 엣지 케이스 | 기대 동작 | 검증 테스트 |
|---|---|---|---|
| 키/값 | 빈 키, NUL·개행·UTF-8 포함, 8KB 키, 접두 관계 키 | 길이 기반으로 구분, 키는 복사 저장 | `test_special_keys`, `test_set_binary_and_utf8`, `test_large_binary_utf8_values`, `test_binary_values_with_nul_and_newlines` |
| 해시 | 모든 키가 같은 해시 | 체인 하나에서도 정확 | `test_full_collision_chain_integrity` |
| 해시 | 리해싱 도중 put/get/delete, 대량 삭제 후 축소, 용량 올림 | 정확성 유지, 2의 거듭제곱 | `test_ops_during_incremental_rehash`, `test_shrink`, `test_initial_capacity_rounds_to_pow2` |
| scan | 순회 중 확장/축소 | 계속 존재한 키는 최소 1회 반환 | `test_scan_returns_all_across_growth/shrink`, `test_scan_consistency` |
| 만료 | 정확히 만료 시각(`now == expire_at`) | 이미 만료로 취급 | `test_px_expiry_boundary_and_lazy`, `test_ttl_with_fake_clock` |
| 만료 | PX ≤ 0, `now+PX` 오버플로, PXAT ≤ 0, 과거 PXAT, `INT64_MAX` PXAT | `MR_INVALID_ARG`/`MR_OVERFLOW`/기존 키 삭제/TTL 있는 키 | `test_set_ttl_edge_values`, `test_set_expire_options` |
| 만료 | 일반 `SET`은 TTL 제거, `PERSIST` 후에는 시간이 지나도 유지 | | `test_set_ttl_edge_values`, `test_ttl_with_fake_clock` |
| 만료 | 아무도 접근하지 않는 만료 키 | cron sweep이 삭제(`expired_keys_active ≥ 50`) | `test_ttl_with_fake_clock`, `test_sweep_*` |
| INCR | 정수 아님, 선행 0·`+`·`-0`·공백, 오버플로, 없는 키 | `-ERR value is not an integer...` / 오버플로 오류, 실패 시 변화 없음 | `test_incr_family`(store/command) |
| glob | `a*a*a*...b` 같은 병적 패턴, 닫히지 않은 `[`, 역순 범위, `\` 이스케이프, NUL 포함 | 지수 폭발 없음(마지막 `*`만 백트래킹) | `test_pathological_patterns_are_fast`, `test_class`, `test_escape`, `test_binary_safe` |
| 프로토콜 | 1바이트씩 도착, 파이프라인 100개, inline, `*0`/`*-1`, 선언 개수 과대 | 부분 입력은 NEED_MORE, 사전 할당 없음 | `test_parse_byte_by_byte`, `test_split_one_byte_at_a_time`, `test_pipeline_100`, `test_parse_empty_multibulk`, `test_parse_no_prealloc_for_declared_count` |
| 프로토콜 | 문법 위반·상한 초과 | `-ERR Protocol error: ...` 후 연결 종료, 파서 오류 고착 | `test_parse_errors`, `test_parse_error_is_sticky`, `test_protocol_error_closes_connection` |
| 프로토콜 | 임의 바이트·변형 입력 | 크래시/UB 없음 | `test_fuzz_split_streams`, `test_fuzz_garbage_and_mutations` (ASan/UBSan 하) |
| 프로토콜 | `QUIT` 뒤 파이프라인 | 이후 명령 미실행 | `test_quit_with_pipelined_tail` |
| 서버 | `maxclients` 초과 | `-ERR max number of clients reached` 후 거절 | `test_maxclients` |
| 서버 | 클라이언트 갑작스런 종료, 느린 클라이언트(출력 상한), 유휴 연결 | 연결 정리/종료, 서버 지속 | `test_abrupt_disconnects`, `test_output_buffer_limit`, `test_idle_timeout` |
| 서버 | 포트 사용 중, stop 요청 | 명확한 오류(`Address already in use`)로 실패 / 루프 종료 | `test_run_stop_and_port_in_use`, smoke |
| 동시성 | 100 클라이언트가 같은 키에 `INCR` | 합이 정확 | `test_100_clients_incr` |
| AOF | 끝 불완전 레코드(모든 절단 위치) | 잘라내고 성공, 이어 쓰기 가능 | `test_truncated_tail_is_cut_at_every_offset` |
| AOF | 중간 손상(쓰레기, 길이 불일치, 모르는 명령, 인자 수, 음수 PXAT, `*0`, `*99`, 과대 길이) | `MR_CORRUPT`, 파일 불변, 서버 시작 거부 | `test_middle_corruption_is_reported`, `test_aof_corrupt_refuses_start` |
| AOF | 재생 중 이미 지난 TTL, 재시작 후 만료 | 로딩 모드로 보존 후 정리(`expired_dropped`) | `test_expired_records_are_dropped`, `test_loading_mode_preserves_past_ttl` |
| AOF | 쓰기/I-O 실패, 반쪽 레코드 | `ftruncate` 롤백, 실패 후 쓰기 차단, `BGREWRITEAOF` 성공 시 해제 | `test_write_failure_rolls_back_partial_record`, `test_io_error_paths`, `test_aof_failure_blocks_writes` |
| AOF | 압축 후 재시작, 대용량 값 | 데이터 동일, 파일 축소 | `test_rewrite_compacts_and_survives_restart`, `test_aof_restart_and_rewrite`, `test_large_value_roundtrip` |
| 메모리 | n번째 할당 하나를 실패시키며(n=1..4999) 반복 | 오류 반환·상태 불변·누수 없음 | `test_alloc_failure_injection`(ht/store/aof), `test_reply_nomem` |

## 한계

- 실제 `redis-cli`/클라이언트 라이브러리 호환은 실행하지 않았다(미확인). 호환성 검증은 테스트 코드의 자체 클라이언트와 bash `/dev/tcp`로만 했다.
- 정전·디스크 가득 참 같은 실제 장애는 주입(할당 실패, I/O 오류 경로, 절단 파일)으로 흉내 냈을 뿐 실기기에서 검증하지 않았다.
- 성능 수치는 별도 벤치([../bench/RESULTS.md](../bench/RESULTS.md))이며 단일 실행이다.
