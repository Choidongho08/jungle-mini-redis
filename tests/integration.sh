#!/usr/bin/env bash
# Mini Redis 통합 테스트 (design §15, TD-008)
#
#   make test-integration                      # build/mini-redis 를 띄워서 테스트
#   SERVER_BIN=build/lsan/mini-redis bash tests/integration.sh
#   TARGET_HOST=host.docker.internal TARGET_PORT=6379 bash tests/integration.sh
#                                              # 서버를 띄우지 않고 실제 Redis 8 에 같은 테스트 실행
#
# 일반 명령은 redis-cli --no-raw 로 보내고 출력 문자열을 비교한다.
# 분할 전송이나 잘못된 프로토콜처럼 redis-cli 로 만들 수 없는 입력은 bash 의 /dev/tcp 로 원시 바이트를 보낸다.

set -u

SERVER_BIN=${SERVER_BIN:-build/mini-redis}
HOST=${TARGET_HOST:-127.0.0.1}
PORT=${TARGET_PORT:-6399}
EXTERNAL=0
if [ -n "${TARGET_HOST:-}" ] || [ -n "${TARGET_PORT:-}" ]; then EXTERNAL=1; fi

PASS=0
FAIL=0
SKIP=0
SERVER_PID=
SERVER_LOG=$(mktemp)

# ---------------------------------------------------------------- 서버 관리

cli() { redis-cli --no-raw -h "$HOST" -p "$PORT" "$@"; }

start_server() {
    # sanitizer 빌드는 이 환경에서 드물게 시작 직후 죽는다(ASLR 문제, Makefile 참고).
    # 시작 단계에서 죽은 경우에만 다시 시도한다.
    for attempt in 1 2 3 4 5; do
        "$SERVER_BIN" --port "$PORT" >"$SERVER_LOG" 2>&1 &
        SERVER_PID=$!
        for _ in $(seq 50); do
            if ! kill -0 "$SERVER_PID" 2>/dev/null; then break; fi
            if cli PING >/dev/null 2>&1; then return 0; fi
            sleep 0.1
        done
        wait "$SERVER_PID" 2>/dev/null
        echo "server failed to start (attempt $attempt)" >&2
    done
    echo "FATAL: could not start $SERVER_BIN" >&2
    exit 2
}

stop_server() {
    [ -z "$SERVER_PID" ] && return 0
    kill -TERM "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID"
    local rc=$?
    SERVER_PID=
    return $rc
}

cleanup() {
    if [ -n "$SERVER_PID" ]; then kill -KILL "$SERVER_PID" 2>/dev/null; fi
    rm -f "$SERVER_LOG"
}
trap cleanup EXIT

# ---------------------------------------------------------------- 단언

section() { printf '\n== %s\n' "$1"; }

ok()   { PASS=$((PASS + 1)); printf '  ok    %s\n' "$1"; }
fail() { FAIL=$((FAIL + 1)); printf '  FAIL  %s\n        expected: %q\n        actual:   %q\n' "$1" "$2" "$3"; }

assert_eq() { # name expected actual
    if [ "$2" = "$3" ]; then ok "$1"; else fail "$1" "$2" "$3"; fi
}

# check NAME EXPECTED cli-args...
check() {
    local name=$1 expected=$2
    shift 2
    assert_eq "$name" "$expected" "$(cli "$@" 2>&1)"
}

# Redis 8 과 호환 대상이 아닌 케이스 (inline 명령 등)
mini_only() {
    if [ "$EXTERNAL" = 1 ]; then
        SKIP=$((SKIP + 1))
        printf '  skip  %s (Mini Redis only)\n' "$1"
        return 1
    fi
    return 0
}

# ---------------------------------------------------------------- 원시 TCP 헬퍼

RAW_FD=

raw_open() { exec {RAW_FD}<>"/dev/tcp/$HOST/$PORT"; }
raw_close() { [ -n "$RAW_FD" ] && exec {RAW_FD}>&- 2>/dev/null; RAW_FD=; }

# raw_send FORMAT : printf %b 형식으로 바이트를 보낸다
raw_send() { printf '%b' "$1" >&"$RAW_FD"; }

# raw_send_slow FORMAT : 1바이트씩 나눠서 보낸다 (분할 수신 검증)
raw_send_slow() {
    local s
    s=$(printf '%b' "$1"; printf x)
    s=${s%x}
    local i
    for ((i = 0; i < ${#s}; i++)); do
        printf '%s' "${s:i:1}" >&"$RAW_FD"
        sleep 0.005
    done
}

# raw_read_lines N : 응답 N 줄을 읽어 '\r' 을 지우고 '|' 로 이어 붙인다
raw_read_lines() {
    local n=$1 out= line
    for ((k = 0; k < n; k++)); do
        if IFS= read -r -t 2 line <&"$RAW_FD"; then
            out+="${line%$'\r'}|"
        else
            out+="<timeout>|"
        fi
    done
    printf '%s' "$out"
}

# raw_is_closed : 서버가 연결을 닫았으면 0 (EOF 를 바로 받음)
raw_is_closed() {
    timeout 2 cat <&"$RAW_FD" >/dev/null 2>&1
    [ $? -ne 124 ]
}

# raw_error_and_close NAME INPUT EXPECTED_LINE : 에러 한 줄을 받고 연결이 닫혀야 한다
raw_error_and_close() {
    raw_open
    raw_send "$2"
    assert_eq "$1: reply" "$3|" "$(raw_read_lines 1)"
    if raw_is_closed; then ok "$1: connection closed"; else fail "$1: connection closed" "closed" "open"; fi
    raw_close
}

# ================================================================ 테스트

if [ "$EXTERNAL" = 0 ]; then
    start_server
    echo "server: $SERVER_BIN on port $PORT (pid $SERVER_PID)"
else
    echo "target: external server $HOST:$PORT"
fi

section "US-001 PING"
check "PING" "PONG" PING
check "PING message" '"hello"' PING hello
check "lowercase ping" "PONG" ping
check "mixed case PiNg" "PONG" PiNg
check "PING too many args" "(error) ERR wrong number of arguments for 'ping' command" PING a b

section "FR-006 unknown command"
check "unknown command" "(error) ERR unknown command 'foo', with args beginning with: 'a' 'b' " foo a b
check "unknown command no args" "(error) ERR unknown command 'foo', with args beginning with: " foo
# 에러 후에도 같은 연결에서 계속 쓸 수 있어야 한다
raw_open
raw_send '*1\r\n$3\r\nfoo\r\n*1\r\n$4\r\nPING\r\n'
assert_eq "connection kept after unknown command" \
    "-ERR unknown command 'foo', with args beginning with: |+PONG|" "$(raw_read_lines 2)"
raw_close

section "FR-004 split / pipelined requests"
raw_open
raw_send_slow '*1\r\n$4\r\nPING\r\n'
assert_eq "PING sent 1 byte at a time" "+PONG|" "$(raw_read_lines 1)"
raw_send_slow '*2\r\n$4\r\nPING\r\n$5\r\nhe\r\nl\r\n'
assert_eq "bulk containing CRLF, split" '$5|he|l|' "$(raw_read_lines 3)"
raw_send '*1\r\n$4\r\nPING\r\n*2\r\n$4\r\nPING\r\n$2\r\nhi\r\n'
assert_eq "two requests in one write" '+PONG|$2|hi|' "$(raw_read_lines 3)"
raw_send '*0\r\n*1\r\n$4\r\nPING\r\n'
assert_eq "empty multibulk ignored" "+PONG|" "$(raw_read_lines 1)"
raw_close

section "FR-005 protocol errors"
if mini_only "inline request rejected"; then
    raw_error_and_close "not starting with *" 'PING\r\n' "-ERR Protocol error: expected '*', got 'P'"
fi
raw_error_and_close "invalid multibulk length" '*abc\r\n' "-ERR Protocol error: invalid multibulk length"
raw_error_and_close "missing \$" '*1\r\nX\r\n' "-ERR Protocol error: expected '\$', got 'X'"
raw_error_and_close "invalid bulk length" '*1\r\n$abc\r\n' "-ERR Protocol error: invalid bulk length"
raw_error_and_close "negative bulk length" '*1\r\n$-5\r\n' "-ERR Protocol error: invalid bulk length"
raw_error_and_close "bulk length over 512MB" '*1\r\n$536870913\r\n' "-ERR Protocol error: invalid bulk length"
big_count=$(printf '*%070000d' 1)
raw_error_and_close "multibulk count line over 64KB" "$big_count" "-ERR Protocol error: too big mbulk count string"
check "other clients unaffected after protocol errors" "PONG" PING

# ================================================================ 결과

section "result"
if [ "$EXTERNAL" = 0 ]; then
    if stop_server; then ok "server exited cleanly on SIGTERM"; else fail "server exited cleanly on SIGTERM" "0" "$?"; fi
    if grep -qE "LeakSanitizer|AddressSanitizer|runtime error" "$SERVER_LOG"; then
        fail "sanitizer report" "none" "$(grep -m3 -E 'LeakSanitizer|AddressSanitizer|runtime error|SUMMARY' "$SERVER_LOG")"
    fi
fi
printf '\npassed: %d  failed: %d  skipped: %d\n' "$PASS" "$FAIL" "$SKIP"
[ "$FAIL" -eq 0 ]
