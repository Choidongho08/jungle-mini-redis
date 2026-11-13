# 해시 테이블 (`mr_ht`)

구현: `src/core/ht.c`, 공개 API: `include/miniredis/ht.h`. 저장소(`store.c`)의 유일한 자료구조이며, 키/값 모두 바이너리 안전(길이 기반)이다. 단일 스레드 전용이라 동기화가 없다.

## 1. 설계 원리

| 항목 | 선택 | 이유 |
|---|---|---|
| 해시 함수 | SipHash-1-3 (`siphash13`), 128비트 시드 | 공격자가 키를 고르는 서버에서 FNV 같은 비키드 해시는 같은 버킷으로 몰리는 키를 미리 계산해 O(n) 체인을 만들 수 있다(해시 플러딩). 서버는 시작 시 `/dev/urandom`으로 시드를 만든다(`random_seed`, `server.c`) |
| 충돌 처리 | 체이닝(단일 연결 리스트) | 구현이 단순하고 삭제가 쉽다. 엔트리는 `next, val, hash, klen, key[]`(flexible array)로 한 번의 `malloc`에 키를 복사해 담는다 |
| 용량 | 항상 2의 거듭제곱(최소 4) | `hash & (cap-1)`로 인덱싱(`%` 불필요) |
| 해시 값 저장 | 엔트리에 `hash` 보관 | 리해싱 때 재해시 불필요, 비교 시 `hash`·`klen`이 다르면 `memcmp` 전에 탈락(`key_eq`) |
| 리사이즈 | 점진적 리해싱: `t[0]`(기존) / `t[1]`(새) | 키가 많을 때 한 번에 옮기면 요청 하나가 수십~100ms 멈춘다. 연산마다 버킷 1개씩 옮겨 지연을 분산 |
| 값 소유권 | `val_free` 콜백이 있으면 테이블이 소유 | 덮어쓰기·삭제·clear·destroy에서 한 곳(`free_entry`)이 해제 |

### 확장/축소 임계

| 조건 | 시점 | 새 용량 | 코드 |
|---|---|---|---|
| 확장 | **삽입 직전** `used >= cap` (로드 팩터 1.0) | `cap_for(used*2)` | `maybe_expand` |
| 축소 | 삭제 직후(및 scan 삭제·리해싱 종료 직후) `used < cap/10` (로드 팩터 < 0.1), `cap > 4` | `cap_for(used*2)` | `maybe_shrink` |
| 리해싱 중 | 새 리사이즈를 시작하지 않는다 | - | 두 함수 모두 `is_rehashing`이면 즉시 반환 |

- 축소 후 로드 팩터가 0.5 이하가 되므로 곧바로 다시 확장되는 진동이 없다(주석: `maybe_shrink`).
- 확장용 버킷 배열 할당이 실패해도(`start_resize`의 `MR_NOMEM`) `put`은 성공한다(체인이 길어질 뿐). 버킷이 하나도 없는 경우(`cap == 0`)만 실패를 돌려준다.
- 빈 테이블(`t[0].used == 0`)의 리사이즈는 리해싱 없이 배열만 즉시 교체한다.

## 2. 점진적 리해싱 단계별 동작

자료구조: `t[0]`, `t[1]`(각각 `buckets, cap, used`)와 `rehash_idx`(다음에 옮길 `t[0]` 버킷, 비활성이면 `HT_NOT_REHASHING`).

예: `cap=4` 테이블에 키 4개가 들어 있고 5번째 키를 `put`한다.

```
(0) 시작           t[0]: cap 4, used 4          rehash_idx = 없음
(1) put(5번째)     rehash_step(1): 리해싱 중 아님 → 아무 일 없음
                   find_link → 없음
                   maybe_expand: used(4) >= cap(4)
                     → start_resize(cap_for(8)=8): t[1] = calloc(8 버킷), rehash_idx = 0
                   새 엔트리는 t[1] 에 삽입 (t[0] 이 아님)      t[0].used 4 / t[1].used 1
(2) 이후 put/get/delete 마다 rehash_step(1)
                   t[0].buckets[rehash_idx] 의 체인 전체를 t[1] 로 이동
                   (idx = hash & (t[1].cap-1), 엔트리의 저장된 hash 사용)
                   빈 버킷은 rehash_idx++ 만 하며 건너뜀, 한 걸음에 최대 10개
(3) 이동 중 조회    find_link 가 t[0] → t[1] 순서로 양쪽을 검사
    이동 중 삭제    찾은 쪽의 used-- (t[which])
    이동 중 삽입    항상 t[1] (이미 비운 t[0] 버킷에 새 원소가 생기지 않게)
(4) t[0].used == 0  finish_rehash: free(t[0].buckets); t[0] = t[1]; t[1] = 빈 테이블;
                   rehash_idx = 없음. 그 뒤 maybe_shrink 검토(리해싱 중 많이 지워졌을 수 있음)
```

규칙 요약:

- `mr_ht_get`도 리해싱을 진행하므로 `const`가 아니다(`mr_ht_contains`도 동일).
- 리해싱 중에는 두 테이블을 모두 보므로 조회 비용이 최대 두 배다. 벤치(`bench/RESULTS.md`)에서 1e6 키 연속 삽입 시 평균 put이 130 → 234 ns로 늘었고, 대신 단일 put 최악 지연은 554.3 us였다(단일 실행 값).
- 리해싱 중에 `used`가 다시 임계를 넘어도 새 확장은 끝난 뒤로 미뤄지며, 그동안 체인이 길어질 수 있다.

## 3. scan: 리해싱 중에도 안전한 커서

`mr_ht_scan(ht, cursor, fn, ud)`는 호출 한 번에 버킷 한 개(리해싱 중이면 작은 테이블 버킷 1개 + 그에 대응하는 큰 테이블 버킷 전부)를 방문하고 다음 커서를 돌려준다. 0에서 시작해 0이 반환되면 끝이다.

**커서 증가 방식(Redis `dictScan`의 역비트 증가)**

```c
v |= ~m0;      /* 마스크 밖 비트를 1 로 채우고 */
v = rev64(v);  /* 비트를 뒤집어 */
v++;           /* 1 을 더한 뒤 */
v = rev64(v);  /* 다시 뒤집는다 */
```

일반 증가는 낮은 비트부터 올라가지만, 이 방식은 **마스크(버킷 인덱스) 범위의 최상위 비트부터** 올라간다.

**왜 리사이즈에도 안전한가**

1. 테이블 크기가 2^k에서 2^(k+1)로 커지면 작은 테이블의 버킷 `i`는 큰 테이블에서 하위 k비트가 `i`인 두 버킷(`i`, `i + 2^k`)으로 갈라진다. 역비트 순서에서는 이미 방문한 커서 값들의 집합이 크기가 바뀌어도 "하위 비트가 같은 버킷들의 묶음"으로 유지되므로, 커서가 가리키는 위치 이전의 버킷에 있던 원소는 새 테이블에서도 이미 방문한 구간에 남는다(헤더 주석의 "접두 구간 유지").
2. 축소(2^(k+1) → 2^k)는 반대로 두 버킷이 하나로 합쳐진다. 합쳐진 버킷은 최악의 경우 다시 방문되므로 **중복은 가능하지만 누락은 없다**.
3. 리해싱 중에는 작은 테이블 버킷 `v & m0`을 훑고, 큰 테이블에서는 하위 비트가 같은 모든 버킷(`do … while (v & (m0 ^ m1))` 루프)을 훑는다. 원소가 `t[0]`에 있든 `t[1]`로 옮겨졌든 같은 호출 안에서 두 곳을 모두 보게 된다.
4. 한 걸음 안에서는 리해싱·리사이즈를 하지 않는다(콜백의 `MR_HT_SCAN_DELETE`로 지운 뒤에만 걸음이 끝난 다음 `maybe_shrink`).

**보장**: 순회 시작부터 끝까지 계속 존재한 원소는 최소 한 번 반환된다. 중간에 추가/삭제된 원소는 반환될 수도, 안 될 수도 있고 중복될 수 있다. 이 성질은 `test_scan_returns_all_across_growth`, `test_scan_returns_all_across_shrink`, `test_scan_without_mutation_is_exact`가 검증한다([testing.md](testing.md)).

`store`는 이 scan 위에서 `KEYS`/`SCAN`/active 만료(`mr_store_sweep`)/무효화(`mr_store_invalidate`)를 구현한다.

## 4. 주요 함수 동작

| 함수 (`src/core/ht.c`) | 동작 |
|---|---|
| `mr_ht_put` | ① `rehash_step(1)` ② 해시 계산 ③ `find_link`로 양쪽 테이블 탐색 → 있으면 값 교체(`old != val`일 때만 `val_free`) ④ 없으면 `maybe_expand` → `malloc(sizeof(ht_entry)+klen)` → 키 복사 → 리해싱 중이면 `t[1]`, 아니면 `t[0]` 버킷 머리에 삽입. 실패 시 테이블 불변, `val` 소유권은 호출자 |
| `mr_ht_get` | `rehash_step(1)` → `find_link` → 빌린 포인터 반환. 없으면 `MR_NOT_FOUND` |
| `mr_ht_delete` | `rehash_step(1)` → `find_link`(이전 노드의 `next` 포인터를 얻음) → 링크 끊기 → `free_entry`(`val_free` 호출) → `maybe_shrink` |
| `rehash_step(n)` | 최대 `n`개 버킷(빈 버킷은 `n*10`개까지 건너뜀)을 `t[0]`→`t[1]`로 이동. `t[0].used == 0`이 되면 `finish_rehash` + `maybe_shrink` |
| `find_link` | `t[0]`부터 체인을 훑어 키가 있는 링크를 반환. 리해싱 중이면 `t[1]`도 검사. 비교는 `hash` → `klen` → `memcmp` 순 |
| `start_resize` / `finish_rehash` | 새 배열 `calloc` 후 `t[1]`로 설정 / `t[0]` 해제 후 `t[1]`을 승격 |
| `mr_ht_scan` | §3. `scan_bucket`이 체인을 훑으며 콜백이 `MR_HT_SCAN_DELETE`를 돌려주면 즉시 제거 |
| `mr_ht_clear` / `mr_ht_destroy` | 양쪽 테이블의 모든 엔트리에 `val_free` 후 해제 |
| `mr_ht_memory_usage` | 구조체 + 버킷 배열 + 엔트리 헤더 + 키 바이트 추정(값 메모리·malloc 오버헤드 제외) |

## 5. 복잡도

| 연산 | 평균 | 최악 | 비고 |
|---|---|---|---|
| put / get / delete | O(1) 상각 (+ 키 길이) | O(체인 길이) | 로드 팩터 < 1 유지. 리해싱 한 걸음은 버킷 1개(+빈 버킷 ≤10) 비용이므로 연산 지연이 O(체인)으로 제한됨 |
| 리사이즈 전체 | O(n) | | n 연산에 걸쳐 분산 |
| scan 한 호출 | O(체인 + 대응 버킷 수) | | |
| clear / destroy | O(n) | | |
| size / capacity / is_rehashing | O(1) | | |

해시 함수가 균등하고 시드가 비밀이면 체인 길이는 평균 1 이하다. 시드를 모르면 충돌 키를 미리 만들 수 없다는 것이 SipHash를 쓴 이유이며, 서버 외의 호출자는 `mr_ht_opts.seed`를 0으로 두면 고정 시드가 된다는 점에 유의한다.

## 6. 엣지 케이스

| 케이스 | 동작 |
|---|---|
| 빈 키(`klen == 0`, `key == NULL` 허용) | 정상 키로 취급, 같은 빈 키는 교체 |
| 키에 NUL·개행·UTF-8·8KB 키 | 길이 기반 비교, 접두 관계(`"a"` vs `"a\0b"`)도 구분 |
| 호출자 버퍼 재사용 | 키는 `put` 때 복사되므로 반환 즉시 재사용 가능 |
| 같은 포인터로 덮어쓰기 | `old != val`일 때만 `val_free`(이중 해제 방지) |
| `put` 실패(`MR_NOMEM`/`MR_OVERFLOW`) | 테이블 불변, `val` 소유권은 호출자 |
| 확장용 배열 할당 실패 | `put`은 성공(체인이 길어질 뿐) |
| 전부 같은 해시(충돌 주입) | 체인 하나에 모든 키가 들어가도 정확성 유지(`test_full_collision_chain_integrity`) |
| 리해싱 중 삭제로 `t[0]`이 비어짐 | `rehash_step`이 `finish_rehash` 후 `maybe_shrink` 재검토 |
| 대량 삭제 | `used < cap/10`이면 축소(`test_shrink`) |
| scan 콜백에서 삭제 | `MR_HT_SCAN_DELETE`만 허용. 콜백 안에서 `put`/`delete` 호출은 금지 |
| `initial_capacity` 과대 | `MR_OVERFLOW` |
| 원소가 0개인 테이블에서 `scan` | 즉시 0 반환 |
