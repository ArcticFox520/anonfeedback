#!/usr/bin/env bash
# End-to-end API, privacy and hardening checks against a real server process.
set -u

SERVER_BIN="${SERVER_BIN:?SERVER_BIN is required}"
STATIC_DIR="${STATIC_DIR:-$(pwd)/static}"
MIGRATIONS_DIR="${MIGRATIONS_DIR:-$(pwd)/migrations}"
ADMIN_TOKEN="integration-test-token-0123456789abcdef"

PASSED=0
FAILED=0
BODY=""
TMP_DIR="$(mktemp -d)"

ok() { PASSED=$((PASSED + 1)); printf '  ok   %s\n' "$1"; }
fail() { FAILED=$((FAILED + 1)); printf '  FAIL %s\n' "$1"; }
check() {
  if [ "$2" = "$3" ]; then ok "$1"; else fail "$1 (expected [$2] got [$3])"; fi
}
check_contains() {
  if grep -qi -- "$2" "$3" 2>/dev/null; then ok "$1"; else fail "$1 (missing [$2])"; fi
}
check_absent() {
  if grep -qi -- "$2" "$3" 2>/dev/null; then fail "$1 (found [$2])"; else ok "$1"; fi
}
check_not_200() {
  if [ "$2" != "200" ]; then ok "$1"; else fail "$1 (got 200)"; fi
}

free_port() {
  python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()'
}

start_server() { # port log_prefix extra_env...
  local port="$1" prefix="$2"
  shift 2
  env -i PATH="$PATH" HOME="$HOME" \
    ADMIN_TOKEN="$ADMIN_TOKEN" \
    LISTEN_ADDRESS=0.0.0.0 LISTEN_PORT="$port" \
    DB_PATH="$TMP_DIR/$prefix/feedback.db" \
    STATIC_DIR="$STATIC_DIR" MIGRATIONS_DIR="$MIGRATIONS_DIR" \
    LOG_LEVEL=debug LOG_TARGET=stderr "$@" \
    "$SERVER_BIN" >"$TMP_DIR/$prefix.out" 2>"$TMP_DIR/$prefix.err" &
  echo $!
}

wait_healthy() {
  local port="$1" attempt=0
  while [ "$attempt" -lt 50 ]; do
    if curl -fsS "http://127.0.0.1:$port/api/v1/health" >/dev/null 2>&1; then
      return 0
    fi
    attempt=$((attempt + 1))
    sleep 0.1
  done
  return 1
}

request() { # method path [curl args...]
  local method="$1" path="$2"
  shift 2
  curl -s -o "$TMP_DIR/body" -w '%{http_code}' -X "$method" "http://127.0.0.1:$PORT$path" "$@"
}

cleanup() {
  for pid in "${PIDS[@]:-}"; do
    kill "$pid" 2>/dev/null || true
  done
  wait 2>/dev/null || true
  rm -rf "$TMP_DIR"
}
PIDS=()
trap cleanup EXIT

printf '== integration tests ==\n'

# ---------------------------------------------------------------- main server
PORT="$(free_port)"
PID="$(start_server "$PORT" main SUBMIT_RATE_MAX=100)"
PIDS+=("$PID")
if ! wait_healthy "$PORT"; then
  printf 'server did not become healthy\n'
  cat "$TMP_DIR/main.err" 2>/dev/null
  exit 1
fi
ok "server started and reports healthy"

# static pages
check "GET / returns 200" "200" "$(request GET /)"
check_contains "GET / serves the submit page" "匿名" "$TMP_DIR/body"
check "GET /admin returns 200" "200" "$(request GET /admin)"
check_contains "GET /admin serves the admin page" "管理" "$TMP_DIR/body"
check "GET /assets/styles.css returns 200" "200" "$(request GET /assets/styles.css)"
check "GET /assets/app.js returns 200" "200" "$(request GET /assets/app.js)"
check "GET /assets/admin.js returns 200" "200" "$(request GET /assets/admin.js)"
check "unknown path returns 404" "404" "$(request GET /nope)"
check "path traversal is blocked" "404" \
  "$(curl -s -o "$TMP_DIR/body" -w '%{http_code}' --path-as-is "http://127.0.0.1:$PORT/assets/%2e%2e/%2e%2e/CMakeLists.txt")"
check_not_200 "raw traversal never succeeds" \
  "$(curl -s -o "$TMP_DIR/body" -w '%{http_code}' --path-as-is "http://127.0.0.1:$PORT/assets/../../CMakeLists.txt")"
check_not_200 "traversal to /etc/passwd is blocked" \
  "$(curl -s -o "$TMP_DIR/body" -w '%{http_code}' --path-as-is "http://127.0.0.1:$PORT/../etc/passwd")"
check "POST to a static path returns 405" "405" "$(request POST /)"

# api basics
check "health endpoint returns 200" "200" "$(request GET /api/v1/health)"
check_contains "health payload" '"status":"ok"' "$TMP_DIR/body"
check "meta endpoint returns 200" "200" "$(request GET /api/v1/meta)"
check_contains "meta lists categories" '"categories"' "$TMP_DIR/body"
check "unknown api path returns 404" "404" "$(request GET /api/v1/nope)"
check "GET on submit endpoint returns 405" "405" "$(request GET /api/v1/feedback)"

# submission validation
check "submit without content type returns 415" "415" \
  "$(request POST /api/v1/feedback -d '{"content":"x"}')"
check "submit with invalid json returns 400" "400" \
  "$(request POST /api/v1/feedback -H 'Content-Type: application/json' -d 'not json')"
check "submit without content returns 400" "400" \
  "$(request POST /api/v1/feedback -H 'Content-Type: application/json' -d '{}')"
check_contains "missing content error code" 'INVALID_CONTENT' "$TMP_DIR/body"
check "submit with blank content returns 400" "400" \
  "$(request POST /api/v1/feedback -H 'Content-Type: application/json' -d '{"content":"   "}')"
check "submit with unknown category returns 400" "400" \
  "$(request POST /api/v1/feedback -H 'Content-Type: application/json' -d '{"content":"hello","category":"spam"}')"
check_contains "unknown category error code" 'INVALID_CATEGORY' "$TMP_DIR/body"

check "submit with 5001 characters returns 400" "400" \
  "$(request POST /api/v1/feedback -H 'Content-Type: application/json' \
      --data-binary "$(python3 -c 'import json;print(json.dumps({"content":"a"*5001}))')")"

BIG_BODY="$(python3 -c 'import json;print(json.dumps({"content":"a"*70000}))')"
check "oversized body returns 413" "413" \
  "$(request POST /api/v1/feedback -H 'Content-Type: application/json' --data-binary "$BIG_BODY")"
check_contains "oversized body error code" 'PAYLOAD_TOO_LARGE' "$TMP_DIR/body"

# successful submissions
check "valid submission returns 201" "201" \
  "$(request POST /api/v1/feedback -H 'Content-Type: application/json' \
      -d '{"content":"  第一条意见 <script>alert(1)</script>  ","category":"bug"}')"
FEEDBACK_ID="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["id"])' "$TMP_DIR/body")"
BUCKET="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["created_at_bucket"])' "$TMP_DIR/body")"
if [ -n "$FEEDBACK_ID" ]; then ok "receipt id returned"; else fail "receipt id returned"; fi
if printf '%s' "$BUCKET" | grep -Eq '^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:00:00Z$'; then
  ok "created_at_bucket is an hour bucket"
else
  fail "created_at_bucket is an hour bucket (got $BUCKET)"
fi
EXPECTED_BUCKET="$(python3 -c 'import datetime;print(datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:00:00Z"))')"
check "hour bucket matches current UTC hour" "$EXPECTED_BUCKET" "$BUCKET"

check "second submission without category returns 201" "201" \
  "$(request POST /api/v1/feedback -H 'Content-Type: application/json' -d '{"content":"第二条意见"}')"

# no cookies, no cors
curl -s -D "$TMP_DIR/headers" -o /dev/null "http://127.0.0.1:$PORT/"
check_absent "no Set-Cookie header" "^set-cookie" "$TMP_DIR/headers"
check_absent "no CORS wildcard header" "access-control-allow-origin" "$TMP_DIR/headers"
check_contains "nosniff header present" "x-content-type-options: nosniff" "$TMP_DIR/headers"
check_contains "CSP header present" "content-security-policy" "$TMP_DIR/headers"
check_contains "referrer policy header present" "referrer-policy: no-referrer" "$TMP_DIR/headers"

# admin authentication gate
check "admin list without token returns 401" "401" "$(request GET /api/v1/admin/feedback)"
check_contains "unauthorized error code" 'UNAUTHORIZED' "$TMP_DIR/body"
check "admin list with wrong token returns 401" "401" \
  "$(request GET /api/v1/admin/feedback -H 'Authorization: Bearer wrong-token-value-here')"
check "admin list with malformed header returns 401" "401" \
  "$(request GET /api/v1/admin/feedback -H "Authorization: $ADMIN_TOKEN")"
check "admin detail without token returns 401" "401" \
  "$(request GET "/api/v1/admin/feedback/$FEEDBACK_ID")"
check "admin delete without token returns 401" "401" \
  "$(request DELETE "/api/v1/admin/feedback/$FEEDBACK_ID")"

AUTH=(-H "Authorization: Bearer $ADMIN_TOKEN")
check "admin list with token returns 200" "200" "$(request GET /api/v1/admin/feedback "${AUTH[@]}")"
TOTAL="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["total"])' "$TMP_DIR/body")"
check "admin list reports both submissions" "2" "$TOTAL"
check_contains "list contains the stored content" '第一条意见' "$TMP_DIR/body"
check_contains "list contains the hour bucket" "$BUCKET" "$TMP_DIR/body"

check "status filter returns 200" "200" "$(request GET '/api/v1/admin/feedback?status=new' "${AUTH[@]}")"
check "invalid status filter returns 400" "400" \
  "$(request GET '/api/v1/admin/feedback?status=bogus' "${AUTH[@]}")"
check "invalid limit returns 400" "400" \
  "$(request GET '/api/v1/admin/feedback?limit=9999' "${AUTH[@]}")"
check "pagination works" "200" "$(request GET '/api/v1/admin/feedback?limit=1&offset=1' "${AUTH[@]}")"
check_contains "pagination echoes limit" '"limit":1' "$TMP_DIR/body"

check "admin detail returns 200" "200" \
  "$(request GET "/api/v1/admin/feedback/$FEEDBACK_ID" "${AUTH[@]}")"
check_contains "admin detail keeps content verbatim" 'alert(1)' "$TMP_DIR/body"
check "unknown id returns 404" "404" \
  "$(request GET "/api/v1/admin/feedback/11111111-1111-4111-8111-111111111111" "${AUTH[@]}")"
check "malformed id returns 404" "404" \
  "$(request GET "/api/v1/admin/feedback/not-a-uuid" "${AUTH[@]}")"

check "status update returns 200" "200" \
  "$(request PATCH "/api/v1/admin/feedback/$FEEDBACK_ID" "${AUTH[@]}" \
      -H 'Content-Type: application/json' -d '{"status":"read"}')"
check_contains "status update is applied" '"status":"read"' "$TMP_DIR/body"
check_contains "updated_at is recorded" '"updated_at"' "$TMP_DIR/body"
check "invalid status update returns 400" "400" \
  "$(request PATCH "/api/v1/admin/feedback/$FEEDBACK_ID" "${AUTH[@]}" \
      -H 'Content-Type: application/json' -d '{"status":"deleted"}')"
check "empty update returns 400" "400" \
  "$(request PATCH "/api/v1/admin/feedback/$FEEDBACK_ID" "${AUTH[@]}" \
      -H 'Content-Type: application/json' -d '{}')"
check_contains "empty update error code" 'EMPTY_UPDATE' "$TMP_DIR/body"
check "oversized note returns 400" "400" \
  "$(request PATCH "/api/v1/admin/feedback/$FEEDBACK_ID" "${AUTH[@]}" \
      -H 'Content-Type: application/json' \
      --data-binary "$(python3 -c 'import json;print(json.dumps({"admin_note":"n"*2001}))')")"
check "note update returns 200" "200" \
  "$(request PATCH "/api/v1/admin/feedback/$FEEDBACK_ID" "${AUTH[@]}" \
      -H 'Content-Type: application/json' -d '{"admin_note":"已处理"}')"
check_contains "note update is applied" '已处理' "$TMP_DIR/body"
check "note can be cleared" "200" \
  "$(request PATCH "/api/v1/admin/feedback/$FEEDBACK_ID" "${AUTH[@]}" \
      -H 'Content-Type: application/json' -d '{"admin_note":null}')"
check_contains "cleared note is null" '"admin_note":null' "$TMP_DIR/body"

check "delete returns 204" "204" "$(request DELETE "/api/v1/admin/feedback/$FEEDBACK_ID" "${AUTH[@]}")"
check "delete twice returns 404" "404" \
  "$(request DELETE "/api/v1/admin/feedback/$FEEDBACK_ID" "${AUTH[@]}")"
check "deleted item is gone" "404" \
  "$(request GET "/api/v1/admin/feedback/$FEEDBACK_ID" "${AUTH[@]}")"

# privacy of logs and database
check_absent "logs contain no opinion text" '第一条意见' "$TMP_DIR/main.err"
check_absent "logs contain no second opinion" '第二条意见' "$TMP_DIR/main.err"
check_absent "logs contain no client address" '127.0.0.1' "$TMP_DIR/main.err"
check_absent "logs contain no admin token" "$ADMIN_TOKEN" "$TMP_DIR/main.err"
check_absent "logs contain no authorization header" 'Authorization' "$TMP_DIR/main.err"

DB_FILE="$TMP_DIR/main/feedback.db"
if [ -f "$DB_FILE" ]; then
  PERMS="$(stat -c '%a' "$DB_FILE")"
  check "database file is 0600" "600" "$PERMS"
  SCHEMA="$(sqlite3 "$DB_FILE" '.schema feedback' | tr 'A-Z' 'a-z')"
  if printf '%s' "$SCHEMA" | grep -Eq '(^|[^a-z_])(ip|ip_address|user_agent|cookie|referer|referrer|session|device|fingerprint)([^a-z_]|$)'; then
    fail "feedback schema has no identity columns"
  else
    ok "feedback schema has no identity columns"
  fi
  ROW_COUNT="$(sqlite3 "$DB_FILE" 'SELECT COUNT(*) FROM feedback;')"
  check "remaining row count after delete" "1" "$ROW_COUNT"
else
  fail "database file exists"
fi

# ------------------------------------------------------------- rate limit server
PORT2="$(free_port)"
PID2="$(start_server "$PORT2" limited SUBMIT_RATE_MAX=2 SUBMIT_RATE_WINDOW_SECONDS=3600)"
PIDS+=("$PID2")
if wait_healthy "$PORT2"; then
  PORT="$PORT2"
  check "rate limit: first submission allowed" "201" \
    "$(request POST /api/v1/feedback -H 'Content-Type: application/json' -d '{"content":"rl-1"}')"
  check "rate limit: second submission allowed" "201" \
    "$(request POST /api/v1/feedback -H 'Content-Type: application/json' -d '{"content":"rl-2"}')"
  check "rate limit: third submission rejected" "429" \
    "$(request POST /api/v1/feedback -H 'Content-Type: application/json' -d '{"content":"rl-3"}')"
  check_contains "rate limit error code" 'RATE_LIMITED' "$TMP_DIR/body"
  if [ -f "$TMP_DIR/limited/feedback.db" ]; then
    STORED="$(sqlite3 "$TMP_DIR/limited/feedback.db" 'SELECT COUNT(*) FROM feedback;')"
    check "rate limited submission is not stored" "2" "$STORED"
  else
    fail "rate limit database exists"
  fi
else
  fail "rate limit server started"
fi

# ------------------------------------------------------------- startup refusals
NO_TOKEN_OUT="$(env -i PATH="$PATH" HOME="$HOME" LISTEN_PORT="$(free_port)" DB_PATH="$TMP_DIR/notoken/feedback.db" \
  STATIC_DIR="$STATIC_DIR" MIGRATIONS_DIR="$MIGRATIONS_DIR" "$SERVER_BIN" 2>&1)"
NO_TOKEN_RC=$?
check "startup fails without ADMIN_TOKEN" "1" "$NO_TOKEN_RC"
if printf '%s' "$NO_TOKEN_OUT" | grep -q 'ADMIN_TOKEN'; then
  ok "startup refusal names ADMIN_TOKEN"
else
  fail "startup refusal names ADMIN_TOKEN"
fi

SHORT_TOKEN_RC=0
env -i PATH="$PATH" HOME="$HOME" ADMIN_TOKEN="tooshort" LISTEN_PORT="$(free_port)" \
  DB_PATH="$TMP_DIR/shorttoken/feedback.db" STATIC_DIR="$STATIC_DIR" \
  MIGRATIONS_DIR="$MIGRATIONS_DIR" "$SERVER_BIN" >/dev/null 2>&1 || SHORT_TOKEN_RC=$?
check "startup fails with a short ADMIN_TOKEN" "1" "$SHORT_TOKEN_RC"

printf '== %d passed, %d failed ==\n' "$PASSED" "$FAILED"
[ "$FAILED" -eq 0 ]
