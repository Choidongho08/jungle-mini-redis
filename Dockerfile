FROM debian:bookworm-slim AS dev

# redis-tools: redis-cli 로 서버에 직접 접속하고 호환성 테스트(CDH/tests/compat)를 돌린다.
# python3: examples/ 의 캐시 예제와 네트워크 벤치마크를 실행한다.
RUN apt-get update \
    && apt-get install -y --no-install-recommends gcc libc6-dev make python3 redis-tools \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

CMD ["bash"]

FROM dev AS build

COPY CDH/ ./CDH/
RUN make -C CDH server BUILD=/tmp/mini-redis-build VARIANT=release

FROM debian:bookworm-slim AS runtime

RUN apt-get update \
    && apt-get install -y --no-install-recommends redis-tools \
    && rm -rf /var/lib/apt/lists/* \
    && install -d -o nobody -g nogroup /data

COPY --from=build /tmp/mini-redis-build/release/mini-redis-server /usr/local/bin/mini-redis-server

USER nobody:nogroup
EXPOSE 6379
ENTRYPOINT ["mini-redis-server"]
CMD ["--bind", "0.0.0.0", "--port", "6379"]
