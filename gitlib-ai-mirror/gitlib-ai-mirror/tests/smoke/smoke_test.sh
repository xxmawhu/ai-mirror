#!/usr/bin/env bash
#
# ai-mirror 冒烟测试
# 包含：
# 1. error 输出检查（log-error-review 规范）
# 2. 行为结果检查（命令执行效果）
# 3. log 完善性检查（日志文件存在与格式）
#
set -euo pipefail

PASS=0
FAIL=0
WARN=0
ERROR_LOG=""
LOG_DIR="/build/log"
HOOK_LOG_DIR="$LOG_DIR/hook"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log_section() { echo -e "${CYAN}=== $1 ===${NC}"; }
ok() {
	echo -e "${GREEN}  [PASS] $1${NC}"
	PASS=$((PASS + 1))
}
fail() {
	echo -e "${RED}  [FAIL] $1${NC}"
	FAIL=$((FAIL + 1))
}
warn() {
	echo -e "${YELLOW}  [WARN] $1${NC}"
	WARN=$((WARN + 1))
}

# ============================================
# Helper: Check for unexpected error/fatal in output
# ============================================
check_no_unexpected_error() {
	local output="$1"
	local context="$2"

	# Check for error/fatal keywords (excluding expected error scenarios)
	# Expected errors: "requires root", "not found" (for missing users)
	local unexpected_errors=$(echo "$output" | grep -E '\[error\].*\[fatal\]' |
		grep -v -E 'requires root|not found|AI user not found|Invalid project path' || true)

	if [ -n "$unexpected_errors" ]; then
		fail "$context: unexpected error/fatal found"
		echo "    Unexpected errors:"
		echo "$unexpected_errors" | head -5 | while read line; do echo "      $line"; done
		ERROR_LOG="$ERROR_LOG$unexpected_errors\n"
	else
		ok "$context: no unexpected error/fatal"
	fi
}

# ============================================
# Helper: Check log file existence and format
# ============================================
check_log_file() {
	local log_file="$1"
	local context="$2"

	if [ ! -f "$log_file" ]; then
		warn "$context: log file not found ($log_file)"
		return
	fi

	# Check log has content
	local lines=$(wc -l <"$log_file" 2>/dev/null || echo "0")
	if [ "$lines" -lt 1 ]; then
		warn "$context: log file empty ($log_file)"
		return
	fi

	# Check log format: should have timestamp prefix (YYYY-MM-DD HH:MM:SS)
	local bad_lines=$(grep -v -E '^\[20[0-9]{2}-[0-9]{2}-[0-9]{2}' "$log_file" | head -3 || true)
	if [ -n "$bad_lines" ]; then
		warn "$context: log format irregular (missing timestamp)"
	fi

	ok "$context: log file valid ($log_file, $lines lines)"
}

# ============================================
# Phase 1: Setup
# ============================================
phase_setup() {
	log_section "Phase 1: Setup"

	mkdir -p "$LOG_DIR" "$HOOK_LOG_DIR"

	# Check binary exists
	if [ ! -f "/usr/local/bin/am" ]; then
		fail "am binary not found"
		exit 1
	fi
	ok "am binary exists"

	# Check config exists
	if [ ! -f "/root/.ai-mirror.toml" ]; then
		fail "config file not found"
		exit 1
	fi
	ok "config file exists"

	# Check SSH keys exist
	if [ ! -f "/root/.ssh/ai-mirror" ]; then
		fail "SSH key not found"
		exit 1
	fi
	ok "SSH key exists"

	# Start sshd
	mkdir -p /run/sshd
	/usr/sbin/sshd 2>/dev/null || true
	sleep 1
	ok "sshd started"
}

# ============================================
# Phase 2: Test am create
# ============================================
test_create() {
	log_section "Phase 2: am create"

	local PROJECT_PATH="/root/projects/testproj"
	local OUTPUT=""
	local AI_USER=""

	# Execute create
	OUTPUT=$(am create "$PROJECT_PATH" 2>&1) || true

	# 1. Error check
	check_no_unexpected_error "$OUTPUT" "am create"

	# 2. Behavior check: extract username
	AI_USER=$(echo "$OUTPUT" | grep -oP 'Created ai-user: \K\S+' || true)
	if [ -z "$AI_USER" ]; then
		fail "Could not extract username from output"
		echo "    Output: $OUTPUT"
		return 1
	fi
	echo "  Created user: $AI_USER"

	# 3. Behavior check: user exists
	if id "$AI_USER" >/dev/null 2>&1; then
		ok "User exists in system"
	else
		fail "User not found in system"
		return 1
	fi

	# 4. Behavior check: username format (prefix + main_user + project hash)
	if [[ "$AI_USER" == "ai_root_testproj"* ]]; then
		ok "Username format correct"
	else
		fail "Username format wrong: $AI_USER"
	fi

	# 5. Behavior check: .am_status exists
	local AI_HOME="/root/projects/testproj"
	if [ -f "$AI_HOME/.am_status" ]; then
		ok ".am_status file exists"

		# Check PoW hash
		local HASH=$(python3 -c "
import json,hashlib
with open('$AI_HOME/.am_status') as f: d=json.load(f)
if 'hash' in d: print(d['hash'][:3])
elif 'nonce' in d: print(hashlib.md5(open('$AI_HOME/.am_status').read().encode()).hexdigest()[:5])
else: print('invalid')
" 2>/dev/null || echo "invalid")
		if [[ "$HASH" == "000"* ]]; then
			ok "PoW hash valid (starts 000)"
		else
			warn "PoW hash unexpected: $HASH"
		fi
	else
		fail ".am_status file missing"
	fi

	# 6. Behavior check: SSH setup
	if [ -f "$AI_HOME/.ssh/authorized_keys" ]; then
		ok "authorized_keys exists"
	else
		fail "authorized_keys missing"
	fi

	# 7. Behavior check: bind mount
	local MOUNT_TARGET="$AI_HOME/.bashrc"
	if findmnt "$MOUNT_TARGET" >/dev/null 2>&1; then
		ok ".bashrc bind mounted"

		# Check read-only
		if findmnt -n -o OPTIONS "$MOUNT_TARGET" | grep -q "ro"; then
			ok "Mount is read-only"
		else
			warn "Mount not read-only"
		fi
	else
		warn ".bashrc not mounted (may need manual update)"
	fi

	# 8. Log check: hook log directory exists
	if [ -d "$HOOK_LOG_DIR" ]; then
		ok "log/hook directory exists"
	else
		warn "log/hook directory missing"
	fi

	# Save AI_USER for later tests
	echo "$AI_USER"
}

# ============================================
# Phase 3: Test am list
# ============================================
test_list() {
	log_section "Phase 3: am list"

	local OUTPUT=""

	OUTPUT=$(am list 2>&1) || true

	# 1. Error check
	check_no_unexpected_error "$OUTPUT" "am list"

	# 2. Behavior check: output contains user info
	if echo "$OUTPUT" | grep -q "ai_root"; then
		ok "list shows ai-user"
	else
		fail "list does not show ai-user"
	fi

	# 3. Behavior check: output format (should have columns)
	if echo "$OUTPUT" | grep -qE 'User|Path|Status'; then
		ok "list output has column headers"
	else
		warn "list output missing headers"
	fi
}

# ============================================
# Phase 4: Test am status
# ============================================
test_status() {
	log_section "Phase 4: am status"

	local OUTPUT=""

	OUTPUT=$(am status 2>&1) || true

	# 1. Error check
	check_no_unexpected_error "$OUTPUT" "am status"

	# 2. Behavior check: shows SSH/Auth status
	if echo "$OUTPUT" | grep -q "SSH"; then
		ok "status shows SSH info"
	else
		warn "status missing SSH info"
	fi

	if echo "$OUTPUT" | grep -q "Auth"; then
		ok "status shows Auth info"
	else
		warn "status missing Auth info"
	fi
}

# ============================================
# Phase 5: Test am health
# ============================================
test_health() {
	log_section "Phase 5: am health"

	local OUTPUT=""
	local EXIT_CODE=0

	OUTPUT=$(am health 2>&1) || EXIT_CODE=$?

	# 1. Error check
	check_no_unexpected_error "$OUTPUT" "am health"

	# 2. Behavior check: exit code (0 = healthy, 1 = unhealthy)
	if [ $EXIT_CODE -eq 0 ]; then
		ok "health exit code 0 (healthy)"
	elif [ $EXIT_CODE -eq 1 ]; then
		warn "health exit code 1 (unhealthy)"
	else
		fail "health unexpected exit code: $EXIT_CODE"
	fi

	# 3. Behavior check: output format
	if echo "$OUTPUT" | grep -qE 'HEALTHY|UNHEALTHY|STALE'; then
		ok "health output has status keywords"
	else
		warn "health output missing status keywords"
	fi

	# 4. Behavior check: no stale mounts detected (fresh setup)
	if echo "$OUTPUT" | grep -q "STALE"; then
		warn "health detected stale mounts (unexpected in fresh setup)"
	else
		ok "no stale mounts detected"
	fi
}

# ============================================
# Phase 6: Test am update (SSH repair)
# ============================================
test_update_ssh() {
	log_section "Phase 6: am update (SSH repair)"

	local AI_USER="${1:-}"
	local AI_HOME="/root/projects/testproj"
	local OUTPUT=""

	if [ -z "$AI_USER" ]; then
		fail "No AI user to test"
		return 1
	fi

	# Tamper .ssh ownership to test repair
	chown root:root "$AI_HOME/.ssh" 2>/dev/null || true

	OUTPUT=$(am update "$AI_HOME" 2>&1) || true

	# 1. Error check
	check_no_unexpected_error "$OUTPUT" "am update"

	# 2. Behavior check: ownership restored
	local AI_UID=$(id -u "$AI_USER")
	local SSH_UID=$(stat -c '%u' "$AI_HOME/.ssh" 2>/dev/null || echo "missing")
	if [ "$SSH_UID" = "$AI_UID" ]; then
		ok ".ssh ownership restored"
	else
		fail ".ssh ownership NOT restored (expected=$AI_UID, got=$SSH_UID)"
	fi

	# 3. Behavior check: mount still exists
	if findmnt "$AI_HOME/.bashrc" >/dev/null 2>&1; then
		ok ".bashrc still mounted after update"
	else
		warn ".bashrc not mounted after update"
	fi

	# 4. Log completeness: check for "Fixing ownership" log
	if echo "$OUTPUT" | grep -q "Fixing ownership"; then
		ok "update logged ownership fix"
	else
		warn "update missing ownership fix log"
	fi
}

# ============================================
# Phase 7: Test stale mount recovery
# ============================================
test_stale_mount_recovery() {
	log_section "Phase 7: stale mount recovery"

	local AI_HOME="/root/projects/testproj"
	local MOUNT_TARGET="$AI_HOME/.bashrc"
	local SOURCE_FILE="/root/.bashrc"
	local OUTPUT=""

	# Verify mount is healthy first
	if ! findmnt "$MOUNT_TARGET" >/dev/null 2>&1; then
		warn "Mount not exists, skipping stale test"
		return
	fi

	if ! cat "$MOUNT_TARGET" >/dev/null 2>&1; then
		warn "Mount already stale, skipping stale test"
		return
	fi

	ok "Mount healthy before stale test"

	# Simulate stale: delete source, recreate
	cp "$SOURCE_FILE" /tmp/.bashrc.stale_backup
	rm -f "$SOURCE_FILE"

	# Mount should become stale (may not work on all filesystems)
	if ! cat "$MOUNT_TARGET" >/dev/null 2>&1; then
		ok "Mount became stale after source deletion"
	else
		warn "Mount did not become stale (filesystem caches)"
	fi

	# Recreate source
	cp /tmp/.bashrc.stale_backup "$SOURCE_FILE"
	rm -f /tmp/.bashrc.stale_backup
	ok "Source file recreated"

	# Run update
	OUTPUT=$(am update "$AI_HOME" 2>&1) || true

	# 1. Error check
	check_no_unexpected_error "$OUTPUT" "stale recovery"

	# 2. Behavior check: mount accessible after update
	if cat "$MOUNT_TARGET" >/dev/null 2>&1; then
		ok "Mount accessible after stale recovery"
	else
		fail "Mount STILL stale after update"
	fi

	# 3. Behavior check: content correct
	if grep -q "test bashrc content" "$MOUNT_TARGET" 2>/dev/null; then
		ok "Mount content correct after recovery"
	else
		fail "Mount content wrong after recovery"
	fi

	# 4. Log check: should have stale mount log
	if echo "$OUTPUT" | grep -q "Stale mount"; then
		ok "stale mount detected and logged"
	else
		warn "missing stale mount log"
	fi
}

# ============================================
# Phase 7.5: Test mount failure diagnostics
# ============================================
test_mount_failure_diagnostics() {
	log_section "Phase 7.5: mount failure diagnostics"

	local AI_HOME="/root/projects/testproj"
	local OUTPUT=""
	local TEMP_SOURCE="/tmp/nonexistent_mount_source_$$"

	# Scenario 1: mount with invalid source (source does not exist)
	# This triggers mount failure → should output detailed diagnostics

	# Add an invalid mount path to config (simulate mount failure)
	# We'll use am update on a path that has a stale mount pointing to nonexistent source

	# First, create a situation where mount will fail:
	# Create a temporary file, mount it, then delete the source
	local TEMP_TARGET="$AI_HOME/.local/bin/test_mount_fail"
	mkdir -p "$AI_HOME/.local/bin"

	# Create temp source file
	echo "test mount source content" >"$TEMP_SOURCE"

	# Manually bind mount (so we can control the scenario)
	mount --bind "$TEMP_SOURCE" "$TEMP_TARGET" 2>/dev/null || {
		warn "Could not create test mount (may need root)"
		rm -f "$TEMP_SOURCE"
		return
	}

	ok "Test mount created: $TEMP_SOURCE -> $TEMP_TARGET"

	# Delete source to make mount stale
	rm -f "$TEMP_SOURCE"
	ok "Source deleted to create stale mount"

	# Verify mount is now stale/inaccessible
	if ! cat "$TEMP_TARGET" >/dev/null 2>&1; then
		ok "Mount became stale (target inaccessible)"
	else
		warn "Mount still accessible (filesystem caches)"
	fi

	# Run am update - should detect stale and attempt recovery
	OUTPUT=$(am update "$AI_HOME" 2>&1) || true

	# 1. Log check: should detect stale mount
	if echo "$OUTPUT" | grep -qE "Stale mount|inode mismatch|target inaccessible"; then
		ok "Stale mount detected in logs"
	else
		warn "Missing stale mount detection log"
	fi

	# 2. Log check: should output diagnostics on mount failure
	# Check for diagnostic info: stat, /proc/mounts, fs type
	local DIAG_COUNT=$(echo "$OUTPUT" | grep -cE "source stat|target stat|/proc/mounts|fs type|mount context" || echo "0")
	if [ "$DIAG_COUNT" -ge 3 ]; then
		ok "Mount failure diagnostics logged (found $DIAG_COUNT diagnostic lines)"
	else
		warn "Mount failure diagnostics incomplete (found $DIAG_COUNT diagnostic lines)"
	fi

	# 3. Behavior check: should attempt umount (even if it fails)
	if echo "$OUTPUT" | grep -qE "Lazy unmounted|umount failed|will attempt remount"; then
		ok "umount attempt logged"
	else
		warn "Missing umount attempt log"
	fi

	# 4. Behavior check: should attempt remount even if umount failed
	# The key fix: umount failure should NOT skip remount attempt
	if echo "$OUTPUT" | grep -qE "Fixing mount|Mount failed"; then
		ok "Remount attempt logged (umount failure did not block remount)"
	else
		# Check if mount was already healthy (umount succeeded)
		if findmnt "$TEMP_TARGET" >/dev/null 2>&1 && cat "$TEMP_TARGET" >/dev/null 2>&1; then
			ok "Mount recovered successfully"
		else
			warn "Missing remount attempt log"
		fi
	fi

	# Cleanup: umount the test mount
	umount -l "$TEMP_TARGET" 2>/dev/null || true
	rm -rf "$AI_HOME/.local/bin/test_mount_fail" 2>/dev/null || true
	rm -f "$TEMP_SOURCE" 2>/dev/null || true

	ok "Test mount cleanup done"
}

# ============================================
# Phase 8: Test am rm
# ============================================
test_rm() {
	log_section "Phase 8: am rm"

	local AI_USER="${1:-}"
	local AI_HOME="/root/projects/testproj"
	local OUTPUT=""
	local MOUNT_TARGET="$AI_HOME/.bashrc"

	if [ -z "$AI_USER" ]; then
		fail "No AI user to test"
		return 1
	fi

	# Verify mount exists before rm
	local MOUNT_EXISTS_BEFORE=0
	if findmnt "$MOUNT_TARGET" >/dev/null 2>&1; then
		MOUNT_EXISTS_BEFORE=1
		ok "Mount exists before am rm"
	else
		warn "Mount does not exist before am rm"
	fi

	OUTPUT=$(am rm "$AI_HOME" 2>&1) || true

	# 1. Error check
	check_no_unexpected_error "$OUTPUT" "am rm"

	# 2. Log check: Step 1 - Terminating processes
	if echo "$OUTPUT" | grep -qE "Terminating processes|Step 1"; then
		ok "rm Step 1: terminate processes logged"
	else
		warn "rm missing Step 1 log (terminate processes)"
	fi

	# 3. Log check: Step 2 - Unmounting bind mounts
	if echo "$OUTPUT" | grep -qE "Unmounting|Unmounted.*mount|Step 2"; then
		ok "rm Step 2: unmount logged"
	else
		warn "rm missing Step 2 log (unmount)"
	fi

	# 4. Behavior check: mount removed after unmount
	if [ $MOUNT_EXISTS_BEFORE -eq 1 ]; then
		if ! findmnt "$MOUNT_TARGET" >/dev/null 2>&1; then
			ok "Mount removed after unmount"
		else
			fail "Mount still exists after unmount"
		fi
	fi

	# 5. Log check: Step 3 - Removing Linux user
	if echo "$OUTPUT" | grep -qE "Removing Linux user|User.*removed|Step 3"; then
		ok "rm Step 3: user removal logged"
	else
		warn "rm missing Step 3 log (user removal)"
	fi

	# 6. Behavior check: user removed
	if ! id "$AI_USER" >/dev/null 2>&1; then
		ok "User removed from system"
	else
		fail "User still exists in system"
	fi

	# 7. Log check: Step 4 - Revoking write access
	if echo "$OUTPUT" | grep -qE "Revoking write|Write access revoked|Step 4"; then
		ok "rm Step 4: revoke write access logged"
	else
		warn "rm missing Step 4 log (revoke write)"
	fi

	# 8. Log check: Step 5 - Cleaning up home
	if echo "$OUTPUT" | grep -qE "Cleaning up.*home|Removed home directory|Step 5"; then
		ok "rm Step 5: home cleanup logged"
	else
		warn "rm missing Step 5 log (home cleanup)"
	fi

	# 9. Behavior check: .am_status removed
	if [ ! -f "$AI_HOME/.am_status" ]; then
		ok ".am_status file removed"
	else
		warn ".am_status still exists"
	fi

	# 10. Behavior check: home directory cleaned
	if [ ! -d "$AI_HOME" ]; then
		ok "Home directory removed"
	else
		# Home may still exist (project dir), but should be cleaned of ai-user files
		if [ ! -d "$AI_HOME/.ssh" ]; then
			ok "Home .ssh directory removed"
		else
			warn "Home .ssh still exists"
		fi
	fi

	# 11. Log check: completion
	if echo "$OUTPUT" | grep -qE "Removed.*project"; then
		ok "rm completion logged"
	else
		warn "rm missing completion log"
	fi
}

# ============================================
# Phase 9: Log completeness check
# ============================================
test_log_completeness() {
	log_section "Phase 9: Log completeness"

	# 1. Check log/hook directory
	if [ -d "$HOOK_LOG_DIR" ]; then
		ok "log/hook directory exists"

		# Check for any log files
		local log_files=$(ls "$HOOK_LOG_DIR"/*.log 2>/dev/null || true)
		if [ -n "$log_files" ]; then
			ok "hook log files exist"

			# Check each log file format
			for f in $log_files; do
				check_log_file "$f" "hook log"
			done
		else
			warn "no hook log files found"
		fi
	else
		warn "log/hook directory missing"
	fi

	# 2. Check install log
	if [ -f "$LOG_DIR/install.log" ]; then
		check_log_file "$LOG_DIR/install.log" "install log"
	else
		warn "install.log not found"
	fi

	# 3. Summary of error log
	if [ -n "$ERROR_LOG" ]; then
		fail "Unexpected errors detected during testing"
		echo "    Summary of unexpected errors:"
		echo "$ERROR_LOG" | head -10
	else
		ok "No unexpected errors in entire test"
	fi
}

# ============================================
# Cleanup
# ============================================
cleanup() {
	log_section "Cleanup"

	pkill sshd 2>/dev/null || true

	# Remove any remaining test users
	for user in $(getent passwd | grep "^ai_" | cut -d: -f1); do
		userdel -r "$user" 2>/dev/null || true
		groupdel "$user" 2>/dev/null || true
	done

	rm -rf /root/projects/testproj/.am_status /root/projects/testproj/.ssh 2>/dev/null || true

	ok "Cleanup done"
}

# ============================================
# Main
# ============================================
main() {
	log_section "ai-mirror Smoke Test"
	echo ""

	phase_setup

	local AI_USER=$(test_create)

	if [ -n "$AI_USER" ] && id "$AI_USER" >/dev/null 2>&1; then
		test_list
		test_status
		test_health
		test_update_ssh "$AI_USER"
		test_stale_mount_recovery
		test_mount_failure_diagnostics
		test_rm "$AI_USER"
	else
		fail "User creation failed, skipping tests"
	fi

	test_log_completeness

	cleanup

	echo ""
	log_section "Results"
	echo -e "  Passed: ${GREEN}${PASS}${NC}"
	echo -e "  Failed: ${RED}${FAIL}${NC}"
	echo -e "  Warnings: ${YELLOW}${WARN}${NC}"
	echo ""

	if [ $FAIL -gt 0 ]; then
		echo -e "${RED}SMOKE TEST FAILED${NC}"
		exit 1
	fi

	echo -e "${GREEN}SMOKE TEST PASSED${NC}"
	exit 0
}

main "$@"
