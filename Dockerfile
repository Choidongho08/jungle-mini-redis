FROM debian:bookworm-slim AS dev

RUN apt-get update \
    && apt-get install -y --no-install-recommends gcc libc6-dev make python3 redis-tools \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

CMD ["bash"]

FROM dev AS build

COPY Makefile ./
COPY src/ ./src/
RUN make BUILD_DIR=/tmp/mini-redis-build

FROM debian:bookworm-slim AS runtime

RUN apt-get update \
    && apt-get install -y --no-install-recommends redis-tools \
    && rm -rf /var/lib/apt/lists/* \
    && install -d -o nobody -g nogroup /data

COPY --from=build /tmp/mini-redis-build/mini-redis /usr/local/bin/mini-redis

USER nobody:nogroup
EXPOSE 6379
ENTRYPOINT ["mini-redis"]
CMD ["0.0.0.0", "6379"]
