#!/usr/bin/env bash
# 실제 서버 바이너리 스모크 테스트.
#
# 외부 도구(redis-cli, nc, python) 없이 bash 의 /dev/tcp 로 RESP 를 직접 주고받는다.
#   1) --help / 잘못된 인자 종료 코드
#   2) 기동 → PING/SET/GET(+inline) → SIGTERM → 종료 코드 0
#   3) 같은 포트로 두 번째 기동 → "Address already in use" 로 비0 종료
#   4) 같은 AOF 로 재기동 → 데이터 복구 확인 → SIGINT → 종료 코드 0
set -u

BIN=${1:?usage: smoke.sh path/to/mini-redis-server}
TMP=$(mktemp -d)
LOG="$TMP/server.log"
PID=""
: >"$LOG"

cleanup() {
    if [ -n "$PID" ]; then
        kill -9 "$PID" 2>/dev/null
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT

fail() {
    echo "SMOKE FAIL: $*" >&2
    echo "--- server log ---" >&2
    cat "$LOG" >&2
    exit 1
}

pass() { echo "  [ OK ] $*"; }

PORT=$((20000 + RANDOM % 20000))

start_server() {
    "$BIN" --port "$PORT" --aof "$TMP/appendonly.aof" --appendfsync always \
        --log-level info "$@" >>"$LOG" 2>&1 &
    PID=$!
    for _ in $(seq 1 200); do
        if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then
            return 0
        fi
        kill -0 "$PID" 2>/dev/null || fail "server exited during startup"
        sleep 0.05
    done
    fail "server did not start listening on port $PORT"
}

stop_server() {
    local sig=$1
    kill "-$sig" "$PID" || fail "kill -$sig failed"
    wait "$PID"
    local rc=$?
    PID=""
    [ "$rc" -eq 0 ] || fail "server exit code $rc after SIG$sig (want 0)"
    pass "SIG$sig -> exit code 0"
}

# 한 줄 응답을 읽어 기대값(끝의 \r 포함)과 비교
expect_line() {
    local want=$1 line=""
    IFS= read -r -t 5 line <&3 || fail "no reply (want '$want')"
    [ "$line" = "$want"$'\r' ] || fail "got '${line%$'\r'}' want '$want'"
}

# --- 1) CLI 인자 ---
"$BIN" --help >/dev/null || fail "--help should exit 0"
"$BIN" --version | grep -q "mini-redis-server" || fail "--version output"
"$BIN" --port 99999 >/dev/null 2>&1
[ $? -eq 2 ] || fail "invalid --port should exit 2"
"$BIN" --bogus x >/dev/null 2>&1
[ $? -eq 2 ] || fail "unknown option should exit 2"
pass "CLI --help/--version/invalid args"

# --- 2) 기동·명령·SIGTERM ---
start_server
exec 3<>"/dev/tcp/127.0.0.1/$PORT" || fail "connect"
printf '*1\r\n$4\r\nPING\r\n' >&3
expect_line "+PONG"
printf '*3\r\n$3\r\nSET\r\n$5\r\nsmoke\r\n$11\r\nhello world\r\n' >&3
expect_line "+OK"
printf '*2\r\n$3\r\nGET\r\n$5\r\nsmoke\r\n' >&3
expect_line '$11'
expect_line "hello world"
printf 'INCR hits\r\nINCR hits\r\n' >&3 # inline + 파이프라인
expect_line ":1"
expect_line ":2"
printf '*2\r\n$3\r\nGET\r\n$7\r\nmissing\r\n' >&3
expect_line '$-1'
printf '*1\r\n$4\r\nQUIT\r\n' >&3
expect_line "+OK"
exec 3>&-
pass "PING/SET/GET/INCR/QUIT over /dev/tcp"

# --- 3) 포트 사용 중 ---
"$BIN" --port "$PORT" >"$TMP/dup.log" 2>&1
rc=$?
[ "$rc" -ne 0 ] || fail "second server on busy port should fail"
grep -q "Address already in use" "$TMP/dup.log" || fail "busy port message: $(cat "$TMP/dup.log")"
pass "busy port -> clear error, exit $rc"

stop_server TERM

# --- 4) AOF 재기동 ---
start_server
exec 3<>"/dev/tcp/127.0.0.1/$PORT" || fail "connect after restart"
printf '*2\r\n$3\r\nGET\r\n$5\r\nsmoke\r\n' >&3
expect_line '$11'
expect_line "hello world"
printf '*2\r\n$3\r\nGET\r\n$4\r\nhits\r\n' >&3
expect_line '$1'
expect_line "2"
exec 3>&-
pass "AOF replay after restart"
stop_server INT

grep -q "AOF loaded" "$LOG" || fail "missing AOF load log line"
echo "smoke: all checks passed"
