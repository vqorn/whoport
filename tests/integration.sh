#!/bin/sh
# End-to-end test: start a real server inside a fake project, find it with
# whoport, then stop it with whoport. Works on Linux and macOS.
set -eu

BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
PORT=${WHOPORT_TEST_PORT:-47913}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/whoport-it.XXXXXX")
PROJECT="$WORK/demo-shop"
SERVER_PID=""

cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok - $*"; }

mkdir -p "$PROJECT/.git" "$PROJECT/src"
cd "$PROJECT/src"

# Port must be free before we start.
if "$BIN" "$PORT" >/dev/null; then fail "port $PORT already in use before the test"; fi
pass "free port reports exit status 1"

python3 -m http.server "$PORT" --bind 127.0.0.1 >/dev/null 2>&1 &
SERVER_PID=$!
for _ in $(seq 1 50); do
    "$BIN" "$PORT" >/dev/null 2>&1 && break
    sleep 0.1
done
if ! "$BIN" "$PORT" >/dev/null; then
    echo "--- whoport --no-color" >&2; "$BIN" --no-color >&2 || true
    echo "--- WHOPORT_DEBUG (last 40 lines)" >&2; WHOPORT_DEBUG=1 "$BIN" --no-color 2>&1 | tail -40 >&2 || true
    echo "--- lsof" >&2; lsof -nP -iTCP -sTCP:LISTEN >&2 2>/dev/null || true
    fail "server on $PORT not detected"
fi
pass "busy port reports exit status 0"

OUT=$("$BIN" --no-color)
echo "$OUT" | grep -q "$PORT" || fail "table does not list port $PORT: $OUT"
echo "$OUT" | grep -q "demo-shop" || fail "table does not show the project folder: $OUT"
echo "$OUT" | grep -q "http.server" || fail "table does not show the command: $OUT"
pass "table shows port, project and command"

DETAIL=$("$BIN" --no-color "$PORT")
echo "$DETAIL" | grep -q "demo-shop" || fail "detail view lacks project: $DETAIL"
echo "$DETAIL" | grep -q "only this computer" || fail "detail view lacks loopback note: $DETAIL"
pass "detail view"

JSON=$("$BIN" --json "$PORT")
echo "$JSON" | python3 -c "
import json, sys
data = json.load(sys.stdin)
assert len(data) == 1, data
e = data[0]
assert e['port'] == $PORT, e
assert e['pid'] == $SERVER_PID, e
assert e['project'].endswith('demo-shop'), e
assert e['memory_bytes'] > 0 and e['uptime_seconds'] >= 0, e
" || fail "json output: $JSON"
pass "json output"

"$BIN" --no-color "$PORT" --kill | grep -q "Port $PORT is free now" || fail "kill did not report success"
sleep 0.2
kill -0 "$SERVER_PID" 2>/dev/null && fail "server still running after --kill"
SERVER_PID=""
if "$BIN" "$PORT" >/dev/null; then fail "port still reported busy after --kill"; fi
pass "--kill stops the server and frees the port"

"$BIN" --bogus >/dev/null 2>&1 && fail "unknown flag accepted"
[ "$("$BIN" --version)" != "" ] || fail "no version"
pass "argument errors"

echo "all integration tests passed"
