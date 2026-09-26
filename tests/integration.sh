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
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        sleep 0.5 # Windows keeps the folder locked until the process is gone
    fi
    rm -rf "$WORK" 2>/dev/null || true
}
trap cleanup EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok - $*"; }

mkdir -p "$PROJECT/.git" "$PROJECT/src"
cd "$PROJECT/src"

# On Windows (MSYS2 / Git Bash) the shell's PIDs differ from Windows PIDs.
case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*) WINDOWS=1 ;;
    *) WINDOWS=0 ;;
esac
if [ "$WINDOWS" = 1 ]; then
    PY=$(command -v python || command -v python3)
else
    PY=$(command -v python3 || command -v python)
fi

# Port must be free before we start.
if "$BIN" "$PORT" >/dev/null; then fail "port $PORT already in use before the test"; fi
pass "free port reports exit status 1"

"$PY" -m http.server "$PORT" --bind 127.0.0.1 >/dev/null 2>&1 &
SERVER_PID=$!
REAL_PID=$SERVER_PID
for _ in $(seq 1 50); do
    "$BIN" "$PORT" >/dev/null 2>&1 && break
    sleep 0.1
done
if ! "$BIN" "$PORT" >/dev/null; then
    echo "--- whoport --no-color" >&2; "$BIN" --no-color >&2 || true
    echo "--- WHOPORT_DEBUG (last 40 lines)" >&2; WHOPORT_DEBUG=1 "$BIN" --no-color 2>&1 | grep -E "lport $PORT|processes with" >&2 || true
    echo "--- lsof" >&2; lsof -nP -iTCP -sTCP:LISTEN 1>&2 || true; echo "--- server pid $SERVER_PID" >&2; ps -p "$SERVER_PID" -o pid,command 1>&2 || true
    fail "server on $PORT not detected"
fi
pass "busy port reports exit status 0"
if [ "$WINDOWS" = 1 ] && [ -r "/proc/$SERVER_PID/winpid" ]; then REAL_PID=$(cat "/proc/$SERVER_PID/winpid"); fi

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
echo "$JSON" | "$PY" -c "
import json, sys
data = json.load(sys.stdin)
assert len(data) == 1, data
e = data[0]
assert e['port'] == $PORT, e
assert e['pid'] == $REAL_PID, e
assert e['project'].endswith('demo-shop'), e
assert e['memory_bytes'] > 0 and e['uptime_seconds'] >= 0, e
" || fail "json output: $JSON"
pass "json output"

FREE=$("$BIN" --free "$PORT")
[ "$FREE" -gt "$PORT" ] 2>/dev/null || fail "--free returned '$FREE', expected a port after the busy $PORT"
[ "$("$BIN" --free "$FREE")" = "$FREE" ] || fail "--free does not return a free start port itself"
pass "--free skips the busy port"

"$BIN" --no-color "$PORT" --kill | grep -q "Port $PORT is free now" || fail "kill did not report success"
for _ in $(seq 1 20); do kill -0 "$SERVER_PID" 2>/dev/null || break; sleep 0.1; done
kill -0 "$SERVER_PID" 2>/dev/null && fail "server still running after --kill"
SERVER_PID=""
if "$BIN" "$PORT" >/dev/null; then fail "port still reported busy after --kill"; fi
pass "--kill stops the server and frees the port"

"$BIN" --bogus >/dev/null 2>&1 && fail "unknown flag accepted"
[ "$("$BIN" --version)" != "" ] || fail "no version"
pass "argument errors"

"$BIN" help | grep -q -- "--kill" || fail "whoport help does not list the commands"
"$BIN" --help | grep -q -- "--free" || fail "whoport --help does not list the commands"
pass "help"

# --wait: returns as soon as a server comes up, and gives up after --timeout.
WPORT=$((PORT + 1))
"$BIN" --wait "$WPORT" --timeout 1 2>/dev/null && fail "--wait succeeded on a free port"
(sleep 1; exec "$PY" -m http.server "$WPORT" --bind 127.0.0.1 >/dev/null 2>&1) &
WAIT_SERVER=$!
"$BIN" --wait "$WPORT" --timeout 20 2>/dev/null || fail "--wait did not see the server come up"
pass "--wait"

# --watch: prints a line when the port closes.
"$BIN" --watch "$WPORT" --no-color >"$WORK/watch.txt" 2>&1 &
WATCH_PID=$!
sleep 2
"$BIN" "$WPORT" --kill --force >/dev/null 2>&1 || true
kill "$WAIT_SERVER" 2>/dev/null || true
for _ in $(seq 1 50); do grep -q "closed" "$WORK/watch.txt" && break; sleep 0.2; done
kill "$WATCH_PID" 2>/dev/null || true
grep -q "in use" "$WORK/watch.txt" || fail "--watch did not report the busy port: $(cat "$WORK/watch.txt")"
grep -q "closed" "$WORK/watch.txt" || fail "--watch did not report the closed port: $(cat "$WORK/watch.txt")"
pass "--watch"

# --live needs a terminal; drive it through a pseudo-terminal where Python has one.
"$BIN" --live </dev/null >/dev/null 2>&1 && fail "--live ran without a terminal"
if [ "$WINDOWS" = 0 ]; then
    "$PY" - "$BIN" <<'PYEOF' || fail "--live did not start and quit cleanly"
import os, pty, select, sys, time
pid, fd = pty.fork()
if pid == 0:
    os.execv(sys.argv[1], [sys.argv[1], "--live"])
out, end = b"", time.time() + 5
while time.time() < end and b"quit" not in out:
    if select.select([fd], [], [], 0.1)[0]:
        out += os.read(fd, 65536)
os.write(fd, b"q")
end = time.time() + 5
while time.time() < end:
    done, status = os.waitpid(pid, os.WNOHANG)
    if done:
        sys.exit(0 if b"whoport" in out and os.waitstatus_to_exitcode(status) == 0 else 1)
    time.sleep(0.1)
sys.exit(1)
PYEOF
fi
pass "--live"

echo "all integration tests passed"
