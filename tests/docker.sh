#!/bin/sh
# Docker end-to-end test (needs a running Docker daemon with Compose):
# starts a Compose project, checks that whoport names the container and its
# project folder, then stops the container through whoport.
set -eu

BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
PORT=${WHOPORT_DOCKER_TEST_PORT:-47922}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/whoport-docker.XXXXXX")
PROJECT="$WORK/demo-stack"

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok - $*"; }
cleanup() {
    (cd "$PROJECT" 2>/dev/null && docker compose down -t 1 >/dev/null 2>&1) || true
    rm -rf "$WORK"
}
trap cleanup EXIT

mkdir -p "$PROJECT"
cat > "$PROJECT/compose.yaml" <<YAML
services:
  web:
    image: nginx:alpine
    ports:
      - "$PORT:80"
YAML
cd "$PROJECT"
docker compose up -d --quiet-pull >/dev/null 2>&1 || docker compose up -d
for _ in $(seq 1 50); do "$BIN" "$PORT" >/dev/null 2>&1 && break; sleep 0.2; done

OUT=$("$BIN" --no-color)
echo "$OUT" | grep -q "container demo-stack-web-1" || fail "table does not name the container: $OUT"
echo "$OUT" | grep "$PORT" | grep -q "demo-stack" || fail "table does not show the Compose folder: $OUT"
pass "table shows container and Compose project"

DETAIL=$("$BIN" --no-color "$PORT")
echo "$DETAIL" | grep -q "published by Docker container demo-stack-web-1" || fail "detail view: $DETAIL"
echo "$DETAIL" | grep -q "nginx:alpine" || fail "detail view lacks the image: $DETAIL"
echo "$DETAIL" | grep -q "demo-stack" || fail "detail view lacks the Compose folder: $DETAIL"
pass "detail view"

"$BIN" --json "$PORT" | grep -q '"name": "demo-stack-web-1"' || fail "json lacks container"
pass "json output"

"$BIN" --no-color "$PORT" --kill | grep -q "Stopped container demo-stack-web-1" || fail "--kill did not stop the container"
[ -z "$(docker ps -q --filter name=demo-stack-web-1)" ] || fail "container still running"
pass "--kill stops the container, not Docker"

echo "all docker tests passed"
