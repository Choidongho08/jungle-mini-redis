# 설계 결정

## 1. 동시성: 싱글 스레드 이벤트 루프

**선택**: 스레드·`fork`·타이머 스레드 없이 `poll()` 루프 하나(`src/server/server.c`)가 소켓 I/O, 명령 실행, 능동 만료, AOF fsync tick을 순서대로 처리한다. Makefile도 `-pthread`를 쓰지 않는다.

**왜 락이 필요 없나**
- 저장소(`mr_store`)와 AOF에 접근하는 코드 경로가 하나뿐이다. 명령은 파싱 → 실행 → AOF 기록 → 응답 작성이 끝난 뒤에야 다음 명령이 시작되므로 명령 단위로 원자적이다.
- 따라서 데드락·경합·메모리 순서 문제가 없다. `INCR` 같은 read-modify-write도 별도 CAS가 필요 없다. 100개 클라이언트가 같은 키에 `INCR`을 파이프라인으로 섞어 보내도 값이 정확하다는 것을 `test_100_clients_incr`가 확인한다(100 클라이언트 × 10회).
- 빌린 포인터(`get`이 돌려주는 값 포인터)의 유효 기간 규칙도 "다음 변경 호출 전까지"로 단순하다.

**한계**
- 느린 명령(큰 `KEYS`, 거대한 값 복사, 동기 `BGREWRITEAOF`)은 그동안 모든 클라이언트를 막는다.
- 한 코어만 사용한다(처리량 확장 불가).
- 완화책: 틱당 연결별 읽기 1MB·명령 1000개 제한으로 한 연결의 독점을 막고, `SCAN`(커서)과 sweep 시간 예산(5ms)으로 긴 작업을 쪼갠다.

## 2. 만료: lazy + active, wall-clock 절대 시각

**데이터**: 엔트리마다 `expire_at`(epoch ms, 0 = 무기한). `now >= expire_at`이면 만료(정확히 그 시각부터 보이지 않음, `is_expired`).

| 방식 | 동작 | 코드 |
|---|---|---|
| lazy | 키에 접근하는 모든 연산이 먼저 만료를 검사해 지운다 | `store.c: lookup` |
| active | cron(100ms)마다 `mr_store_sweep(1000개, 5ms)`가 저장된 커서에서 이어서 `mr_ht_scan`으로 훑으며 만료 키 삭제. 커서가 0으로 돌아오면(한 바퀴) 멈춤 | `store.c: mr_store_sweep` |

- **별도 만료 인덱스(힙 등)를 두지 않았다.** 힙은 `expire/persist/del`마다 O(log n) 갱신과 위치 추적 메모리가 필요하고, Redis식 별도 expires 사전은 키당 두 번째 엔트리가 필요하다. 대신 TTL 키 수(`volatile_keys`)가 0이면 sweep이 즉시 반환해 TTL이 없는 워크로드는 비용이 없다(벤치: TTL 키 없을 때 sweep 호출 1.6 ns). 단점은 한 바퀴 비용이 전체 키 수에 비례한다는 것이다.
- **wall-clock vs monotonic**: 만료 시각은 프로세스 재시작 후에도 의미가 있어야 하고 AOF에 절대 시각으로 기록되므로 `CLOCK_REALTIME`(`wall_ms`)을 쓴다. 대가로 시스템 시계가 뒤로 가면 키가 더 오래 살고 앞으로 가면 일찍 만료된다(Redis와 동일). `CLOCK_MONOTONIC`(`mono_ms`)은 재부팅 간 의미가 없어 저장하지 않고 sweep 시간 예산, fsync 주기, cron, idle timeout 같은 "경과 시간"에만 쓴다.
- **상대 TTL → 절대 시각**: `SET ... EX/PX`, `EXPIRE`는 명령 계층에서 절대 시각으로 변환해 저장소(`PXAT`/`PEXPIREAT`)와 AOF에 같은 값을 쓴다. 그래서 재생 시점과 무관하게 결과가 같다.
- **0 이하 TTL**: `EXPIRE k 0`/`-1`은 Redis처럼 키를 즉시 삭제하고 AOF에는 `DEL`로 기록한다.
- **`KEYS`/`SCAN`은 만료 키를 건너뛰기만** 하고 지우지 않는다(순회 중 축소로 같은 키가 중복 반환되는 것을 막기 위해).
- `TTL`은 반올림(1500ms 남으면 2), 키 없음 -2, 무기한 -1.

## 3. 데이터 무효화

| 수단 | 동작 | AOF 기록 |
|---|---|---|
| `DEL key...` | 키별 삭제, 삭제 수 반환 | 삭제된 키마다 `DEL` |
| `EXPIRE`/`PEXPIRE`/`EXPIREAT`/`PEXPIREAT` | 시간 기반 무효화 | `PEXPIREAT` (0 이하/과거면 `DEL`) |
| `INVALIDATE <glob>` | glob에 맞는 키 전부 삭제, 개수 반환 | `INVALIDATE <glob>` |
| `INVALIDATE PREFIX <prefix>` | 접두사로 시작하는 키 전부 삭제(메타문자 이스케이프 불필요) | `INVALIDATEPREFIX <prefix>` |
| `FLUSHALL` | 전부 삭제 | `FLUSHALL` |

- `INVALIDATE`는 `mr_ht_scan` 한 바퀴를 돌며 콜백의 `MR_HT_SCAN_DELETE`로 삭제한다. 삭제 중 축소로 일부 키를 두 번 보더라도 이미 지워져 무해하고, 계속 존재한 키를 놓치지 않는다는 scan 보장에 기댄다. 순회 중 발견한 만료 키도 같이 지운다(개수에는 포함하지 않음).
- 하나도 지우지 못한 `INVALIDATE`는 AOF에 기록하지 않는다(재생 결과가 같으므로).
- 복잡도는 O(n)이므로 큰 저장소에서는 서버를 그동안 막는다. 접두사 설계(`item:`, `user:1:` 등)로 무효화 범위를 키 이름에 담는 것을 권장한다. 효과 예시는 `bench/RESULTS.md`의 시나리오 E(쓰기 후 `del` + 주기적 접두사 무효화로 hit 70.7%, 미사용 대비 2.73x).

## 4. 영속화: AOF

### 형식

레코드 하나 = RESP 배열 하나(`*N\r\n$len\r\n<bytes>\r\n...`). 길이 기반이라 NUL·개행이 든 값도 안전하고, 요청 형식과 같아 `cat`으로 읽어 디버깅하기 쉽다. 기록되는 정규형(재생기가 이해하는 명령의 전부):

```
SET k v            SET k v PXAT <epoch-ms>
DEL k              PEXPIREAT k <epoch-ms>      PERSIST k
FLUSHALL           INVALIDATE <glob>           INVALIDATEPREFIX <prefix>
```

상대 TTL은 절대 시각으로, `INCR`류는 **결과값 `SET`(+기존 만료 시각 유지)** 으로 기록해 재생이 결정적이다. 쓰기 명령은 저장소 변경이 성공한 뒤에만 기록하고, 기록이 성공한 뒤에 성공 응답을 만든다.

### fsync 정책 트레이드오프

| 정책 | 동작 | 유실 가능 범위 | 비용 |
|---|---|---|---|
| `always` | 매 레코드 `fsync` | 응답 받은 쓰기는 디스크에 반영(하드웨어가 거짓말하지 않는 한) | 쓰기마다 디스크 동기화로 가장 느림 |
| `everysec`(기본) | cron이 `mr_aof_tick`으로 마지막 fsync 후 1초 경과 + 미동기화 데이터가 있을 때 fsync | 최대 약 1초 | 낮음 |
| `no` | OS 페이지 캐시에 맡김(종료 시에도 fsync 안 함) | OS 정책에 따름(미확인: 정량 측정 없음) | 가장 낮음 |

정상 종료(`SIGINT`/`SIGTERM`)는 `mr_aof_close`에서 fsync(`no` 제외) 후 닫는다. `kill -9`·전원 차단은 위 표의 범위만큼 잃을 수 있다. `always`에서 fsync가 실패하면 디스크 상태를 알 수 없으므로 AOF를 `broken`으로 표시해 더 쓰지 않는다(`everysec`의 tick fsync 실패는 아래 쓰기 차단 정책으로 처리).

### 복구(재생)

`mr_aof_open`이 파일을 처음부터 읽어 저장소에 적용한다. 서버는 **재생을 끝낸 뒤에 포트를 연다.**

| 상황 | 동작 |
|---|---|
| 정상 | 모든 레코드 적용 후 이어 쓰기(`O_APPEND`) |
| 끝의 불완전 레코드(쓰다 죽은 흔적) | 완결 레코드 끝으로 `ftruncate` + fsync 후 성공, 로그에 WARN(`truncated N bytes`). 모든 절단 위치에서 검증(`test_truncated_tail_is_cut_at_every_offset`) |
| 중간 손상(문법·알 수 없는 명령·인자 수·길이 불일치) | `MR_CORRUPT`, 서버가 **시작을 거부**하고 파일은 건드리지 않음(`test_middle_corruption_is_reported`, `test_aof_corrupt_refuses_start`) |
| 재생 중 이미 지난 TTL | 로딩 모드(`mr_store_set_loading`)에서는 만료 검사를 하지 않고 보존 → 이후 `PERSIST`된 키를 올바르게 복원. 로딩이 끝나면 만료된 키를 한꺼번에 정리(`expired_dropped`) |

### 압축(`BGREWRITEAOF`, `mr_aof_rewrite`)

현재 살아 있는 키만 `<path>.rewrite.tmp`에 `SET`(+`PXAT`) 레코드로 쓰고 → fsync → `rename`으로 원자 교체 → 부모 디렉터리 fsync(rename 자체의 영속화) → 새 파일의 fd로 이어 쓴다. 실패하면 임시 파일을 지우고 기존 파일을 그대로 쓴다. **동기 실행**이라 키 수에 비례해 서버가 멈춘다(INFO의 `aof_rewrite_mode:synchronous`). 자동 트리거는 없다.

### AOF 기록 실패 시 쓰기 차단 정책

저장소에는 undo가 없어서 메모리는 이미 바뀐 뒤 AOF 기록이 실패하면 되돌릴 수 없다. 그래서:

1. 해당 명령은 `-ERR AOF write failed (...)` 오류로 응답한다.
2. `aof_write_failed`를 세워 이후 **쓰기 명령은 모두 거부**한다(`-ERR AOF write error earlier; ...`). 읽기 명령은 계속 동작한다.
3. cron의 fsync 실패도 같은 플래그를 세운다.
4. `BGREWRITEAOF`가 성공하면(새 파일이 현재 메모리 전체를 담으므로 메모리와 파일이 다시 일치) 플래그를 해제한다. `BGREWRITEAOF`는 쓰기 명령 플래그가 아니어서 차단 중에도 실행된다. `INFO persistence`의 `aof_last_write_status:err`로 확인한다(`test_aof_failure_blocks_writes`).
5. 파일 쓰기가 중간에 실패하면 쓰기 전 길이로 `ftruncate`해 반쪽 레코드를 남기지 않는다(`test_write_failure_rolls_back_partial_record`). 되돌리기조차 실패하면 이후 모든 쓰기를 `MR_IO_ERROR`로 거부한다.

알려진 제약: 다중 키 `DEL`에서 중간 키의 기록이 실패하면 앞선 키는 이미 삭제·기록된 상태에서 오류를 응답한다(코드상 `cmd_del`).

## 5. 외부 API: RESP2

- **왜 RESP2**: `redis-cli`와 대부분의 Redis 클라이언트 라이브러리가 이해하는 사실상 표준이고, 텍스트 기반에 길이 접두라 파서가 작고 바이너리 안전하다. 새 프로토콜을 정의하는 비용(클라이언트 작성)을 피한다. 같은 형식을 AOF에도 재사용했다.
- **입력**: multibulk와 inline(telnet/`nc` 편의) 둘 다 받는다. 파서는 상태 기계라 바이트가 1개씩 와도 O(n)이며, 선언된 인자 수만큼 미리 할당하지 않는다(메모리 DoS 방지). 상한: bulk 64MB, 인자 1,048,576개, inline 64KB. 위반은 복구 불가능한 프로토콜 오류로 연결을 닫는다.
- **응답**: simple string, error, integer, bulk, null bulk(`$-1`), array. simple/error 문자열 안의 `\r\n`은 공백으로 치환해 프레임을 보호한다.
- **오류 형식**: Redis 7과 같은 메시지(`unknown command '...'`, `wrong number of arguments for '...' command`, `value is not an integer or out of range` 등).
- **확장 시 고려**
  - 새 명령은 `command.c`의 `COMMANDS[]`에 `{이름, arity, 핸들러, 플래그}`만 추가하면 되며, 쓰기 명령은 `CMD_WRITE` 플래그(AOF 차단 대상)와 **AOF 정규형 레코드 + 재생기(`apply_record`) 지원**을 함께 추가해야 한다.
  - 새 데이터 타입은 `store_entry`가 문자열 전용이므로 타입 태그 도입이 필요하다.
  - RESP3, 파이프라인 외 트랜잭션(`MULTI`), Pub/Sub, 인증, TLS는 현재 코드에 없다.
  - 인스턴스 정보는 `INFO`(섹션: server, clients, memory, persistence, stats, keyspace)와 읽기 전용 `CONFIG GET`으로 노출한다.

## 6. 대안 비교

| 대안 | 장점 | 채택하지 않은 이유 |
|---|---|---|
| **RDB식 스냅샷** | 파일이 작고 로딩이 빠름 | 마지막 스냅샷 이후 쓰기를 모두 잃는다. 안전하게 만들려면 `fork`(copy-on-write)가 필요한데 이 서버는 fork·스레드를 쓰지 않는다. AOF는 구현이 단순하고 fsync 정책으로 유실 범위를 조절할 수 있다. 스냅샷 대용으로 `BGREWRITEAOF` 압축을 둔다(단, 동기) |
| **멀티스레드(락 기반)** | 여러 코어 활용 | 전역 락이면 이점이 없고 샤딩 락이면 `KEYS`/`SCAN`/무효화/AOF 순서 보장이 복잡해진다. 락·경합·AOF 기록 순서 문제를 피하고 명령 원자성을 공짜로 얻는 쪽을 택했다 |
| **I/O만 멀티스레드, 명령은 단일** | 소켓 I/O 병렬화 | 이 규모에서 복잡도 대비 이득이 불명확하고 측정하지 않았다(미확인) |
| **epoll** | 연결 수가 많을 때 `poll`의 O(n) 순회보다 유리 | 이식성과 단순함을 위해 `poll`을 썼다. `maxclients` 기본 10000에서의 영향은 측정하지 않았다(미확인) |
| **만료 힙/별도 expires 사전** | active 만료가 O(만료 수) | §2 참고: 갱신 비용과 메모리 증가. 대신 한 바퀴 비용이 전체 키 수에 비례 |
| **stop-the-world 리해싱** | 구현 단순 | 대형 테이블에서 단일 요청 지연 급증. 점진적 리해싱으로 최악 지연을 분산(수치와 한계는 `bench/RESULTS.md`; stop-the-world 구현을 만들어 직접 비교한 것은 아님) |
