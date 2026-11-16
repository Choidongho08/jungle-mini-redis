# Mini Redis (CDH)

C11로 작성한 싱글 스레드 인메모리 키-값 서버. Redis의 RESP2 프로토콜로 통신하며, 직접 구현한 해시 테이블(체이닝 + 점진적 리해싱) 위에 TTL 만료, 데이터 무효화, AOF 영속화를 얹었다. 서버가 노출하는 명령은 9개(`PING`, `SET [EX]`, `GET`, `DEL`, `EXISTS`, `EXPIRE`, `TTL`, `INCR`, `FLUSHALL`)다. 외부 라이브러리 의존성은 없고(libc/POSIX만 사용) 빌드는 Makefile 하나로 끝난다.

> 이 문서의 모든 명령은 **Docker 컨테이너(Debian 12, gcc 12)** 에서 실행하는 것을 전제로 한다. 개발 PC(Windows 호스트)에는 gcc가 없으므로 호스트에서 `make`를 직접 실행하지 않는다. 컨테이너 준비는 [저장소 루트 README](../README.md)를 따른다.

## 특징

- **해시 테이블**: SipHash-1-3(시작 시 무작위 시드), 충돌 체이닝, 2의 거듭제곱 버킷, 한 번에 버킷 1개씩 옮기는 점진적 리해싱, 리해싱 중에도 안전한 커서 scan(코어 API)
- **싱글 스레드 이벤트 루프**: `poll()` 하나로 I/O·명령·만료·fsync를 처리. 락이 없고 명령 단위로 원자적
- **만료**: lazy(접근 시) + active(주기적 sweep). 만료 시각은 절대 epoch ms
- **무효화**: `DEL`, `EXPIRE`, `FLUSHALL` (코어에는 glob/접두사 무효화 API가 있으나 서버 명령으로는 노출하지 않는다)
- **AOF 영속화**: RESP 형식 로그, fsync 정책 3종(always/everysec/no), 재시작 시 복구(끝 불완전 레코드 자동 절단). 코어의 압축 API(`mr_aof_rewrite`)를 부르는 서버 명령은 없다
- **프로토콜**: RESP2 multibulk + inline, 파이프라인, 바이너리 안전 키/값
- **테스트**: 단위 6개 스위트(73 케이스) + 기능 1개 스위트(16 케이스, 실제 TCP) + 스모크, ASan/UBSan 빌드, 할당 실패 주입

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
printf 'SET user:1 alice EX 60\r\nGET user:1\r\nTTL user:1\r\nINCR hits\r\nEXISTS user:1 hits\r\nDEL user:1\r\nEXISTS user:1\r\n' >&3
timeout 3 cat <&3
```

명령 사양(`tests/unit/test_command.c`)에서 기대되는 응답은 다음과 같다. 명령을 9개로 줄인 뒤 이 예시를 컨테이너에서 직접 실행해 확인하지는 못했다(미확인):

```
+OK
$5
alice
:60
:1
:2
:1
:0
```

`SIGINT`/`SIGTERM`으로 종료하면 남은 출력을 보내고 AOF를 fsync한 뒤 종료 코드 0으로 끝난다.

> **미확인**: 컨테이너에 `redis-cli`가 없어 `redis-cli`/클라이언트 라이브러리로의 실제 접속은 검증하지 않았다. 검증한 접속 경로는 bash `/dev/tcp`(스모크 테스트)와 테스트 코드의 자체 TCP 클라이언트뿐이다. `COMMAND`/`CONFIG`/`INFO`/`SELECT`/`QUIT`은 지원하지 않아 unknown command 오류로 응답하므로, 접속 시 이런 명령을 보내는 클라이언트가 이 서버에서 어떻게 동작하는지는 확인하지 않았다.
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

명령 이름은 대소문자를 구분하지 않는다. 테이블에 9개. 이 밖의 이름(`ECHO`, `SELECT`, `QUIT`, `KEYS`, `SCAN`, `DBSIZE`, `INFO`, `COMMAND`, `CONFIG`, `PEXPIRE`, `PTTL`, `PERSIST`, `DECR` 등)은 `-ERR unknown command '...', with args beginning with: ...` 로 응답하고 연결은 유지된다.

| 명령 | 응답 | 비고 |
|---|---|---|
| `PING [msg]` | `+PONG`, 또는 msg 를 bulk 로 | |
| `SET key value [EX seconds]` | `+OK` | `EX` 는 1 이상 정수(아니면 `-ERR invalid expire time in 'set' command`), 중복·그 밖의 옵션(`NX`/`XX`/`PX`/`EXAT`/`PXAT` 포함)은 `-ERR syntax error`. 옵션 없는 `SET` 은 기존 TTL 을 지운다 |
| `GET key` | bulk, 없으면 `$-1` | |
| `DEL key [key ...]` | 지운 키 수 | |
| `EXISTS key [key ...]` | 존재하는 키 수 | 같은 키를 여러 번 주면 여러 번 센다 |
| `EXPIRE key seconds` | 적용하면 `:1`, 키 없으면 `:0` | 0 이하는 키를 즉시 삭제(`:1`) |
| `TTL key` | 남은 초(반올림) | 키 없음 `-2`, 무기한 `-1` |
| `INCR key` | 증가한 값 | 64비트 부호 정수, 엄격한 10진 표현만 허용, 없는 키는 0 에서 시작. TTL 유지 |
| `FLUSHALL [ASYNC\|SYNC]` | `+OK` | 싱글 스레드라 옵션과 무관하게 동기 |

코어 라이브러리(`src/core`)는 이 명령 밖의 기능(NX/XX·PX 계열 SET 옵션, PERSIST, INCRBY, KEYS/SCAN, glob 무효화, AOF 압축 등)을 그대로 갖고 있고 단위 테스트도 유지되지만, 서버 명령 계층이 노출하지 않는다.

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

명령을 9개로 줄인 뒤의 컨테이너 실행 결과는 [docs/testing.md](docs/testing.md)의 "실제 실행 결과" 절에 있다. 케이스 목록과 엣지 케이스도 같은 문서에 있다.

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
- **단일 스레드**: 한 코어만 사용하고, 느린 명령(`FLUSHALL`, 거대한 값 복사 등)은 모든 클라이언트를 막는다.
- **AOF 압축 없음**: 코어에 `mr_aof_rewrite`가 있지만 서버가 호출하지 않는다(명령도 자동 트리거도 없음). AOF 파일은 계속 커지며 시작 시 전체를 재생한다.
- **메모리 상한/축출 없음**: `maxmemory`와 eviction 정책이 코드에 없다. 키가 계속 늘면 메모리도 계속 늘어난다.
- **기능 범위**: 문자열 타입만, DB 1개, 명령 9개, 인증·TLS 없음, RDB 스냅샷 없음, 서버 상태 조회(`INFO`/`CONFIG`) 없음.
- **active 만료 한계**: 별도 만료 인덱스 없이 메인 테이블을 커서로 훑으므로 한 바퀴 비용이 전체 키 수에 비례한다.
- **AOF 쓰기 실패 시**: 메모리가 파일보다 앞서므로 쓰기 명령을 막고, 이를 해제하는 명령이 없으므로 **서버를 재시작해야 복구**된다(AOF 재생으로 파일 기준 상태로 돌아간다; 자세한 내용은 [design-decisions.md](docs/design-decisions.md)).

## 문서

| 문서 | 내용 |
|---|---|
| [docs/architecture.md](docs/architecture.md) | 모듈 의존, 요청 처리 흐름 |
| [docs/hash-table.md](docs/hash-table.md) | 해시 테이블 설계·리해싱·scan·주요 함수 |
| [docs/design-decisions.md](docs/design-decisions.md) | 동시성, 만료, 무효화, AOF, 외부 API, 대안 비교 |
| [docs/testing.md](docs/testing.md) | 테스트 전략, 케이스, 엣지 케이스, 실행 결과 |
| [bench/RESULTS.md](bench/RESULTS.md) | 벤치마크 전체 결과 |
