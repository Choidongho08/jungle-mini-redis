# Jungle Mini Redis

팀원은 Docker로 Debian 12, GCC, Make가 포함된 C 개발 환경을 실행할 수 있습니다.

## 1. Docker 준비

macOS와 Windows에서는 Docker Desktop을 실행합니다. Windows에서는 Docker Desktop의 WSL 통합을 켜고 WSL 터미널에서 아래 명령을 실행합니다. Linux에서는 Docker Engine과 Docker Compose가 설치되어 있어야 합니다.

```sh
docker info
docker compose version
```

`docker info`에서 서버에 연결할 수 없다고 나오면 Docker Desktop 또는 Docker Engine이 실행 중인지 확인합니다.

## 2. 저장소 받기 및 이미지 빌드

```sh
git clone https://github.com/Choidongho08/jungle-mini-redis.git
cd jungle-mini-redis
docker compose build dev
```

이미 클론했다면 `compose.yaml`이 있는 저장소 루트에서 빌드 명령만 실행합니다.

## 3. 개발 셸 실행

```sh
HOST_UID=$(id -u) HOST_GID=$(id -g) docker compose run --rm dev
```

셸은 컨테이너의 `/workspace`에서 시작합니다. 이 경로는 로컬 저장소와 연결되어 있어 로컬 편집기에서 저장한 파일을 컨테이너에서 바로 사용할 수 있습니다. `HOST_UID`와 `HOST_GID`는 컨테이너가 로컬 사용자와 같은 숫자 ID로 파일을 만들도록 설정합니다.

컨테이너 안에서 GCC와 Make가 설치되어 있는지 확인합니다.

```sh
gcc --version
make --version
```

`exit`를 입력하면 셸이 종료되고 `--rm` 옵션이 컨테이너를 삭제합니다. 저장소에 저장한 파일은 로컬에 남습니다. 다음에 접속할 때는 개발 셸 실행 명령을 다시 사용합니다. `Dockerfile`이 바뀌면 접속 전에 `docker compose build dev`를 다시 실행합니다.

## 4. Mini Redis (feat/kunho-mini-redis)

C11로 구현한 단일 스레드 Mini Redis입니다. epoll 이벤트 루프, RESP2, SipHash 해시 테이블, lazy expiration을 사용합니다.

지원 명령: `PING` `SET [EX]` `GET` `DEL` `EXISTS` `EXPIRE` `TTL` `INCR` `FLUSHALL`

개발 셸(`/workspace`) 안에서 실행합니다. `Dockerfile`에 `redis-tools`가 추가되었으므로 처음 한 번은 `docker compose build dev`로 이미지를 다시 빌드합니다.

```sh
make                         # build/mini-redis
./build/mini-redis           # 기본 포트 6380 (--port N, -v 로 접속 로그)
redis-cli -p 6380 ping       # 다른 셸에서 접속
```

테스트:

```sh
make test-unit               # SipHash 공식 벡터, dict, 정수 파싱
make test-integration        # 서버를 띄워 redis-cli / 원시 TCP 로 검증
make test                    # 둘 다
make asan                    # AddressSanitizer + UBSan 빌드로 make test
```

같은 통합 테스트를 실제 Redis에 돌려 응답을 비교할 수 있습니다. 대상 서버의 데이터는 `FLUSHALL`로 지워지므로 테스트용 인스턴스를 사용합니다.

```sh
# 호스트에서: docker run --rm -d --name redis-compare -p 6381:6379 redis:8
TARGET_HOST=host.docker.internal TARGET_PORT=6381 bash tests/integration.sh
```

참고: WSL2 같은 최신 커널에서는 GCC 12의 ASan이 시작 직후 무작위로 죽을 수 있습니다. `make asan`은 이런 시작 직후의 크래시만 골라서 다시 실행합니다(`tests/run_sanitized.sh`). 근본적으로 해결하려면 호스트에서 `sysctl -w vm.mmap_rnd_bits=28`을 실행합니다.
