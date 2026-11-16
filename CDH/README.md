# Mini Redis (CDH)

C11로 작성한 싱글 스레드 인메모리 키-값 서버. Redis의 RESP2 프로토콜로 통신하며, 직접 구현한 해시 테이블(체이닝 + 점진적 리해싱) 위에 TTL 만료, 데이터 무효화, AOF 영속화를 얹었다. 외부 라이브러리 의존성은 없고(libc/POSIX만 사용) 빌드는 Makefile 하나로 끝난다.

> 이 문서의 모든 명령은 **Docker 컨테이너(Debian 12, gcc 12)** 에서 실행하는 것을 전제로 한다. 개발 PC(Windows 호스트)에는 gcc가 없으므로 호스트에서 `make`를 직접 실행하지 않는다. 컨테이너 준비는 [저장소 루트 README](../README.md)를 따른다.

## 특징

- **해시 테이블**: SipHash-1-3(시작 시 무작위 시드), 충돌 체이닝, 2의 거듭제곱 버킷, 한 번에 버킷 1개씩 옮기는 점진적 리해싱, 리해싱 중에도 안전한 커서 `SCAN`
- **싱글 스레드 이벤트 루프**: `poll()` 하나로 I/O·명령·만료·fsync를 처리. 락이 없고 명령 단위로 원자적
- **만료**: lazy(접근 시) + active(주기적 sweep). 만료 시각은 절대 epoch ms
- **무효화**: `DEL`, `EXPIRE` 계열, `INVALIDATE <glob>`, `INVALIDATE PREFIX <prefix>`
- **AOF 영속화**: RESP 형식 로그, fsync 정책 3종(always/everysec/no), 재시작 시 복구(끝 불완전 레코드 자동 절단), 압축(`BGREWRITEAOF`)
- **프로토콜**: RESP2 multibulk + inline, 파이프라인, 바이너리 안전 키/값
- **테스트**: 단위 6개 스위트(76 케이스) + 기능 1개 스위트(17 케이스, 실제 TCP) + 스모크, ASan/UBSan 빌드, 할당 실패 주입

## 빠른 시작

저장소 루트에서 실행한다(이미지가 없으면 먼저 `docker compose build dev`).

```sh
docker compose build dev
docker compose run --rm dev make -C CDH help     # 타깃 목록
docker compose run --rm dev make -C CDH test     # 빌드 + 단위 + 기능 테스트
docker compose run --rm dev make -C CDH server   # 서버 바이너리만 빌드
```

산출물은 컨테이너의 `/workspace/build/cdh/<variant>/`(호스트의 `build/cdh/`)에 생긴다. Windows 셸에서 경로가 변환되어 실패하면 `MSYS_NO_PATHCONV=1`을 앞에 붙인다.

### 서버 기동과 접속

컨테이너 셸 안에서 서버를 띄우고 bash `/dev/tcp`로 RESP를 직접 보내 본다.

```sh
docker compose run --rm dev bash
# --- 이하 컨테이너 안 ---
make -C CDH server
build/cdh/default/mini-redis-server --port 6379 --aof /tmp/appendonly.aof &

exec 3<>/dev/tcp/127.0.0.1/6379
printf 'SET user:1 alice EX 60\r\nGET user:1\r\nTTL user:1\r\nINVALIDATE PREFIX user:\r\nDBSIZE\r\nQUIT\r\n' >&3
timeout 3 cat <&3
```

실제 실행 출력(이 문서 작성 중 컨테이너에서 확인):

```
+OK
$5
alice
:60
:1
:0
+OK
```

`SIGINT`/`SIGTERM`으로 종료하면 남은 출력을 보내고 AOF를 fsync한 뒤 종료 코드 0으로 끝난다.

> **미확인**: 컨테이너에 `redis-cli`가 없어 `redis-cli`/클라이언트 라이브러리로의 실제 접속은 검증하지 않았다. 검증한 접속 경로는 bash `/dev/tcp`(스모크 테스트와 위 예시)와 테스트 코드의 자체 TCP 클라이언트뿐이다. `COMMAND DOCS`에는 빈 배열로 응답하도록 구현되어 있어 `redis-cli`가 붙을 수는 있을 것으로 보이나 실행해 보지는 않았다.
> 컨테이너 밖(호스트)에서 접속하려면 `--bind 0.0.0.0`과 포트 매핑이 필요한데, `compose.yaml`의 포트 설정은 확인하지 않았다.

## CLI 옵션

`mini-redis-server [options]` (`--opt value`, `--opt=value` 모두 가능)

| 옵션 | 기본값 | 설명 |
|---|---|---|
| `--port N` | 6379 | TCP 포트(0 = 임의 포트) |
| `--bind ADDR` | 127.0.0.1 | 바인드 주소 |
| `--aof PATH` | (끔) | AOF 파일 경로. 지정하면 시작 시 재생하고 쓰기를 기록 |
| `--appendfsync always\|everysec\|no` | everysec | AOF fsync 정책 |
| `--maxclients N` | 10000 | 동시 연결 상한(1~1000000). 초과 연결은 `-ERR max number of clients reached` 후 종료 |
| `--timeout SEC` | 0(끔) | 유휴 연결 종료 시간 |
| `--log-level debug\|info\|warn\|error` | info | stderr 로그 수준 |
| `--help`, `--version` | | 도움말 / 버전(`mini-redis-server 1.0.0`) |

잘못된 옵션은 종료 코드 2, 시작 실패(포트 사용 중, AOF 손상 등)는 1이다.

## 지원 명령

명령 이름은 대소문자를 구분하지 않는다. 테이블에 28개(`COMMAND COUNT`).

| 분류 | 명령 | 비고 |
|---|---|---|
| 연결 | `PING [msg]`, `ECHO msg`, `QUIT`, `SELECT 0` | DB는 하나뿐, 0 이외는 오류 |
| 문자열 | `SET key value [NX\|XX] [EX\|PX\|EXAT\|PXAT n]`, `GET`, `DEL key...`, `EXISTS key...` | |
| 카운터 | `INCR`, `DECR`, `INCRBY`, `DECRBY` | 64비트 부호 정수, 엄격한 10진 표현만 허용 |
| 만료 | `EXPIRE`, `PEXPIRE`, `EXPIREAT`, `PEXPIREAT`, `TTL`, `PTTL`, `PERSIST` | 0 이하 TTL은 키 즉시 삭제 |
| 순회 | `KEYS pattern`, `SCAN cursor [MATCH p] [COUNT n]`, `DBSIZE` | `KEYS`는 전체를 막는다 → 큰 데이터는 `SCAN` |
| 무효화 | `INVALIDATE glob`, `INVALIDATE PREFIX prefix`, `FLUSHALL [ASYNC\|SYNC]` | `INVALIDATE`는 삭제한 키 수를 반환 |
| 운영 | `INFO [section]`, `BGREWRITEAOF`, `COMMAND [COUNT\|LIST]`, `CONFIG GET` | `CONFIG SET/RESETSTAT/REWRITE`는 미지원 오류 |

glob 문법: `*`, `?`, `[abc]`, `[a-z]`, `[^a]`, `\x`. `INFO` 섹션: server, clients, memory, persistence, stats, keyspace.

## 디렉터리 구조

```
CDH/
├── Makefile                 빌드/테스트 타깃 (bench/bench.mk 를 include)
├── include/miniredis/       공개 헤더 (ht, store, aof, glob, clock, status, resp, command, server)
├── src/
│   ├── core/                ht.c store.c aof.c glob.c clock.c status.c   (소켓을 모르는 코어)
│   ├── server/              resp.c command.c server.c log.c               (프로토콜·이벤트 루프)
│   └── app/main.c           CLI 진입점
├── tests/
│   ├── support/minitest.h   최소 테스트 프레임워크 + 할당 실패 주입
│   ├── unit/                test_ht test_store test_aof test_glob test_resp test_command
│   └── functional/          test_server.c (실제 TCP), smoke.sh (실제 바이너리)
├── bench/                   bench_core.c bench_cache.c bench_common.h bench.mk RESULTS.md
└── docs/                    architecture.md hash-table.md design-decisions.md testing.md
```

## 테스트

```sh
docker compose run --rm dev make -C CDH test     # 단위 + 기능
docker compose run --rm dev make -C CDH utest    # 단위만
docker compose run --rm dev make -C CDH ftest    # 기능(TCP)만
docker compose run --rm dev make -C CDH asan     # ASan+UBSan(누수 검사 포함)으로 test
docker compose run --rm dev make -C CDH smoke    # 실제 서버 바이너리 + bash /dev/tcp
docker compose run --rm dev make -C CDH debug   # -O0 -g 로 test
docker compose run --rm dev make -C CDH release # -O2 -DNDEBUG 로 test
```

최근 실행 결과(컨테이너, 이 문서 작성 시점): `test` 단위 6/6 스위트·기능 1/1 스위트 통과, `asan` 동일하게 통과, `smoke` 전 항목 통과(`smoke: all checks passed`). 케이스 목록과 엣지 케이스는 [docs/testing.md](docs/testing.md).

## 벤치마크

```sh
docker compose run --rm dev make -C CDH -f Makefile -f bench/bench.mk bench
# 개별: bench-core, bench-cache  (인자: BENCH_CORE_ARGS="--quick", BENCH_CACHE_ARGS="--origin-latency-ms 5 ...")
```

`CDH/Makefile`이 `bench/bench.mk`를 include하므로 `make -C CDH bench`도 같은 타깃을 가리키지만, 아래 수치는 위 `-f` 형태로 측정되었다. 전체 표와 해석은 [bench/RESULTS.md](bench/RESULTS.md).

핵심 결과(WSL2 위 Docker, **단일 실행**, in-process 측정):

| 항목 | 값 |
|---|---|
| 해시 테이블 get(hit) 처리량 | 1만 키 29,398,276 / 10만 키 18,495,652 / 100만 키 3,647,151 ops/s |
| 1e6 키 연속 삽입, 단일 put 최악 지연 | 점진적 리해싱 554.3 us (기준선, 리해싱 없음: 85.4 us) |
| 캐시 미사용 (원본 5 ms, 3,000 요청) | 196 req/s, 총 15.31 s |
| 캐시 사용, TTL 김 (hit 81.9%) | 1,084 req/s, 총 2.77 s → **5.53x** |
| 캐시 사용, TTL 300 ms (hit 52.5%) | 412 req/s → 2.10x |
| 쓰기 10% + 키 삭제/접두사 무효화 (hit 70.7%) | 536 req/s → 2.73x (미사용 대비) |

**한계**: 배수는 hit rate와 원본 지연(여기서는 인위적 5 ms `nanosleep`)에 비례하므로 일반화할 수 없다. 단일 클라이언트 순차 요청이며, 네트워크·RESP 파싱·AOF fsync는 포함하지 않는다. 지연 분포·캐시 시나리오는 1회 측정이라 실행 간 편차를 확인하지 않았다.

## 한계와 향후 과제

코드에서 확인된 것만 적는다.

- **복제 없음**: 단일 노드. 서버 다운 시 가용성은 AOF 재시작으로만 확보한다.
- **단일 스레드**: 한 코어만 사용하고, 느린 명령(큰 `KEYS`, 거대한 값 복사 등)은 모든 클라이언트를 막는다.
- **`BGREWRITEAOF`는 동기 실행**: 이름과 달리 백그라운드가 아니다. fork/스레드가 없어 끝날 때까지 서버 전체가 멈춘다(`INFO`의 `aof_rewrite_mode:synchronous`). AOF 자동 압축도 없어 수동 호출이 필요하다.
- **메모리 상한/축출 없음**: `maxmemory`와 eviction 정책이 코드에 없다. 키가 계속 늘면 메모리도 계속 늘어난다.
- **기능 범위**: 문자열 타입만, DB 1개, 인증·TLS 없음, RDB 스냅샷 없음, `CONFIG SET` 미지원.
- **active 만료 한계**: 별도 만료 인덱스 없이 메인 테이블을 커서로 훑으므로 한 바퀴 비용이 전체 키 수에 비례한다.
- **AOF 쓰기 실패 시**: 메모리가 파일보다 앞서므로 쓰기 명령을 막고 `BGREWRITEAOF` 성공 전까지 유지한다(자세한 내용은 [design-decisions.md](docs/design-decisions.md)).

## 문서

| 문서 | 내용 |
|---|---|
| [docs/architecture.md](docs/architecture.md) | 모듈 의존, 요청 처리 흐름 |
| [docs/hash-table.md](docs/hash-table.md) | 해시 테이블 설계·리해싱·scan·주요 함수 |
| [docs/design-decisions.md](docs/design-decisions.md) | 동시성, 만료, 무효화, AOF, 외부 API, 대안 비교 |
| [docs/testing.md](docs/testing.md) | 테스트 전략, 케이스, 엣지 케이스, 실행 결과 |
| [bench/RESULTS.md](bench/RESULTS.md) | 벤치마크 전체 결과 |
