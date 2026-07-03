#!/bin/bash
# tests/docker/test_am_status_security.sh — .am_status security hardening tests
# Verifies that verify_state_safe, rebuild_state, and the wrapper's AI user
# detection correctly handle all edge cases.
set -euo pipefail

LOG_DIR="/var/log/ai-mirror-tests"
mkdir -p "$LOG_DIR"
BIN="${BIN:-/usr/local/bin/ai-mirror-bin}"
WRAPPER="${WRAPPER:-/usr/local/bin/am}"
TEST_HOME="/tmp/am-status-test-$$"
STATE_FILE="$TEST_HOME/.am_status"

pass_count=0
fail_count=0

pass() {
  local name="$1"
  echo "[PASS] $name"
  echo "[PASS] $name" >> "$LOG_DIR/test_am_status_security.log"
  pass_count=$((pass_count + 1))
}

fail() {
  local name="$1"
  echo "[FAIL] $name"
  echo "[FAIL] $name" >> "$LOG_DIR/test_am_status_security.log"
  fail_count=$((fail_count + 1))
}

check_rejected() {
  local name="$1"
  local output="$2"
  if echo "$output" | grep -qiE "corrupted|unsafe|rejected|mismatch|not found|error"; then
    pass "$name"
  else
    echo "  Output: $output" >&2
    fail "$name"
  fi
}

setup() {
  rm -rf "$TEST_HOME"
  mkdir -p "$TEST_HOME"
  cat > "$STATE_FILE" << 'STATE'
{
  "username": "itestuser_abc123",
  "uid": 12345,
  "gid": 12345,
  "home_dir": "/tmp/am-status-test-$$",
  "main_user": "root",
  "project_path": "/tmp/am-status-test-$$/project",
  "path_hash": "abc123",
  "mounts": []
}
STATE
  sed -i "s|\$\$|$$|g" "$STATE_FILE"
}

cleanup() {
  rm -rf "$TEST_HOME"
}

# === Test 1: Empty .am_status → read_state returns error ===
test_empty_file() {
  setup
  > "$STATE_FILE"
  local out
  out=$("$BIN" update "$TEST_HOME" 2>&1 || true)
  check_rejected "empty .am_status" "$out"
  cleanup
}

# === Test 2: Invalid JSON → read_state returns error ===
test_invalid_json() {
  setup
  echo "not json { broken" > "$STATE_FILE"
  local out
  out=$("$BIN" update "$TEST_HOME" 2>&1 || true)
  check_rejected "invalid JSON" "$out"
  cleanup
}

# === Test 3: Symlink .am_status → rejected (O_NOFOLLOW) ===
test_symlink() {
  setup
  rm -f "$STATE_FILE"
  ln -s /etc/passwd "$STATE_FILE"
  local out
  out=$("$BIN" update "$TEST_HOME" 2>&1 || true)
  check_rejected "symlink .am_status" "$out"
  cleanup
}

# === Test 4: home_dir="/etc" → rejected ===
test_home_dir_tamper() {
  setup
  sed -i 's|"home_dir": "[^"]*"|"home_dir": "/etc"|' "$STATE_FILE"
  local out
  out=$("$BIN" update "$TEST_HOME" 2>&1 || true)
  check_rejected "home_dir tamper" "$out"
  cleanup
}

# === Test 5: Non-existent username → rejected ===
test_bad_username() {
  setup
  sed -i 's/"username": "[^"]*"/"username": "no_such_user_xxxxxx"/' "$STATE_FILE"
  local out
  out=$("$BIN" update "$TEST_HOME" 2>&1 || true)
  check_rejected "bad username" "$out"
  cleanup
}

# === Test 6: uid=0 (root) → rejected ===
test_root_uid() {
  setup
  sed -i 's/"uid": [0-9]*/"uid": 0/' "$STATE_FILE"
  sed -i 's/"gid": [0-9]*/"gid": 0/' "$STATE_FILE"
  local out
  out=$("$BIN" update "$TEST_HOME" 2>&1 || true)
  check_rejected "root uid" "$out"
  cleanup
}

# === Test 7: Non-existent main_user → rejected ===
test_bad_main_user() {
  setup
  sed -i 's/"main_user": "[^"]*"/"main_user": "no_such_user"/' "$STATE_FILE"
  local out
  out=$("$BIN" update "$TEST_HOME" 2>&1 || true)
  check_rejected "bad main_user" "$out"
  cleanup
}

# === Test 8: Wrapper blocks AI user ===
test_wrapper_block() {
  local test_user="testai_abc123"
  id "$test_user" &>/dev/null && userdel -r "$test_user" 2>/dev/null || true
  useradd -m -s /bin/bash "$test_user"
  echo '{"username":"testai_abc123","uid":0,"gid":0,"home_dir":"/home/testai_abc123","main_user":"root","project_path":"/p","path_hash":"abc123","mounts":[]}' > "/home/$test_user/.am_status"
  chown -R "$test_user:$test_user" "/home/$test_user"

  local out
  out=$(su - "$test_user" -c "$WRAPPER --version" 2>&1 || true)
  if echo "$out" | grep -qi "cannot use\|error"; then
    pass "AI user blocked from 'am'"
  else
    echo "  Output: $out" >&2
    fail "AI user was not blocked from 'am'"
  fi

  userdel -r "$test_user" 2>/dev/null || true
  rm -rf "/home/$test_user"
}

main() {
  echo "========================================"
  echo ".am_status Security Hardening Tests"
  echo "========================================"
  echo "Binary: $BIN"
  echo ""

  # Verify we have a real binary
  if ! "$BIN" --help 2>&1 | grep -qi "update"; then
    echo "SKIP: binary does not support 'update' subcommand"
    echo "  Build and install the real ai-mirror-bin, then re-run."
    echo "  Dev build: /mnt/beegfs_data/usr/maxx/dev/aimirror/ai-mirror/build/bin/ai-mirror-bin"
    exit 0
  fi

  test_empty_file
  test_invalid_json
  test_symlink
  test_home_dir_tamper
  test_bad_username
  test_root_uid
  test_bad_main_user
  test_wrapper_block

  echo ""
  echo "========================================"
  echo "Results: $pass_count passed, $fail_count failed"
  echo "========================================"
  echo "$pass_count / $((pass_count + fail_count)) tests passed" >> "$LOG_DIR/test_am_status_security.log"
  exit "$fail_count"
}

main "$@"
