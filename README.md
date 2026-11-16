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

## 4. Mini Redis 서버 실행

구현은 [`CDH/`](CDH/README.md)에 있습니다. 서버 이미지를 빌드해 백그라운드로 띄웁니다.

```sh
docker compose up -d --build --wait
redis-cli -p 6379 ping        # 호스트에 redis-cli 가 없으면 아래 명령을 사용
docker compose exec mini-redis redis-cli ping
```

데이터는 `mini-redis-data` 볼륨의 AOF에 저장되어 컨테이너를 다시 만들어도 남습니다. 호스트 포트는 `MINI_REDIS_PORT=6390 docker compose up -d`처럼 바꿀 수 있습니다. 볼륨까지 지우려면 `docker compose down -v`를 실행합니다.

동작을 비교할 실제 Redis 8은 `docker compose --profile redis up -d --wait redis`로 띄웁니다(호스트 포트 `6380`, 개발 셸에서는 `redis-cli -h redis`). 빌드와 테스트 명령은 [`CDH/README.md`](CDH/README.md)를 참고합니다.
