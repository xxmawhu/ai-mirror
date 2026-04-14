#!/usr/bin/env bash
#
# ai-mirror Docker integration test
# Tests: create user, SSH key, bind mount, permissions, list, health, rm, force-destroy
#
set -euo pipefail

PASS=0
FAIL=0
ERRORS=""

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[0;33m'
NC='\033[0m'

log_pass() {
	PASS=$((PASS + 1))
	echo -e "${GREEN}[PASS]${NC} $1"
}
log_fail() {
	FAIL=$((FAIL + 1))
	ERRORS="${ERRORS}\n  FAIL: $1"
	echo -e "${RED}[FAIL]${NC} $1"
}
log_info() { echo -e "${YELLOW}[INFO]${NC} $1"; }

assert_eq() {
	local desc="$1" expected="$2" actual="$3"
	if [ "$expected" = "$actual" ]; then
		log_pass "$desc"
	else
		log_fail "$desc (expected='$expected', actual='$actual')"
	fi
}

assert_contains() {
	local desc="$1" haystack="$2" needle="$3"
	if echo "$haystack" | grep -q "$needle"; then
		log_pass "$desc"
	else
		log_fail "$desc (output does not contain '$needle')"
	fi
}

assert_file_exists() {
	local desc="$1" path="$2"
	if [ -f "$path" ]; then log_pass "$desc"; else log_fail "$desc (file not found: $path)"; fi
}

assert_dir_exists() {
	local desc="$1" path="$2"
	if [ -d "$path" ]; then log_pass "$desc"; else log_fail "$desc (dir not found: $path)"; fi
}

assert_user_exists() {
	local desc="$1" username="$2"
	if id "$username" &>/dev/null; then log_pass "$desc"; else log_fail "$desc (user '$username' does not exist)"; fi
}

assert_user_not_exists() {
	local desc="$1" username="$2"
	if id "$username" &>/dev/null; then log_fail "$desc (user '$username' still exists)"; else log_pass "$desc"; fi
}

# ============================================================
# Setup
# ============================================================
log_info "=== ai-mirror Docker Integration Test ==="

AI_MIRROR="/usr/local/bin/ai-mirror"
PROJECT_DIR="/tmp/test-project-alpha"
PROJECT_DIR2="/tmp/test-project-beta"

if [ ! -x "$AI_MIRROR" ]; then
	echo "ERROR: $AI_MIRROR not found. Run install.sh first."
	exit 1
fi

MAIN_USER="maxx"
useradd --create-home --home-dir "/home/$MAIN_USER" --shell /bin/bash "$MAIN_USER" 2>/dev/null || true
usermod -aG ai-mirror "$MAIN_USER" 2>/dev/null || true

mkdir -p "$PROJECT_DIR" "$PROJECT_DIR2"
chown "$MAIN_USER:$MAIN_USER" "$PROJECT_DIR" "$PROJECT_DIR2"

touch "/home/$MAIN_USER/.bashrc"
chown "$MAIN_USER:$MAIN_USER" "/home/$MAIN_USER/.bashrc"
mkdir -p "/home/$MAIN_USER/.config"
chown "$MAIN_USER:$MAIN_USER" "/home/$MAIN_USER/.config"
mkdir -p "/home/$MAIN_USER/.ssh"
chown "$MAIN_USER:$MAIN_USER" "/home/$MAIN_USER/.ssh"

SUDOERS_FILE="/etc/ai-mirror/sudoers.d/ai-mirror"
if [ -f "$SUDOERS_FILE" ]; then
	echo "#include $SUDOERS_FILE" >>/etc/sudoers.d/ai-mirror-include 2>/dev/null || true
	visudo -cf "$SUDOERS_FILE" 2>/dev/null || log_info "sudoers file validation failed"
fi

# Helper: run ai-mirror as root with SUDO_USER set to MAIN_USER
# This simulates: MAIN_USER runs "sudo ai-mirror ..."
run_as_user() {
	SUDO_USER="$MAIN_USER" "$AI_MIRROR" "$@" 2>&1
}

# ============================================================
# Test 1: config command
# ============================================================
log_info "--- Test 1: config command ---"
OUT=$(SUDO_USER="$MAIN_USER" "$AI_MIRROR" config 2>&1)
assert_contains "config shows prefix" "$OUT" "prefix"
assert_contains "config shows key_type" "$OUT" "ed25519"

# ============================================================
# Test 2: create ai-user
# ============================================================
log_info "--- Test 2: create ai-user ---"
OUT=$(run_as_user create "$PROJECT_DIR")
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')
log_info "Created ai-user: '$AI_USER'"

if [ -z "$AI_USER" ]; then
	log_fail "create returned empty username"
	log_info "Full output: $OUT"
else
	assert_user_exists "ai-user exists after create" "$AI_USER"
	AI_HOME="/home/$AI_USER"
	assert_dir_exists "ai-user home exists" "$AI_HOME"
fi

# ============================================================
# Test 3: SSH key generation
# ============================================================
log_info "--- Test 3: SSH key generation ---"
if [ -n "$AI_USER" ]; then
	SSH_KEY="/home/$MAIN_USER/.ssh/ai-mirror"
	assert_file_exists "SSH private key generated" "$SSH_KEY"
	assert_file_exists "SSH public key generated" "${SSH_KEY}.pub"

	AUTH_KEYS="$AI_HOME/.ssh/authorized_keys"
	assert_file_exists "authorized_keys created" "$AUTH_KEYS"

	if [ -f "${SSH_KEY}.pub" ] && [ -f "$AUTH_KEYS" ]; then
		PUB_KEY=$(cat "${SSH_KEY}.pub")
		if grep -qF "$PUB_KEY" "$AUTH_KEYS" 2>/dev/null; then
			log_pass "public key is in authorized_keys"
		else
			log_fail "public key not found in authorized_keys"
		fi
	fi

	if [ -f "$AUTH_KEYS" ]; then
		PERMS=$(stat -c '%a' "$AUTH_KEYS" 2>/dev/null)
		assert_eq "authorized_keys mode is 600" "600" "$PERMS"
	fi

	SSH_DIR="$AI_HOME/.ssh"
	if [ -d "$SSH_DIR" ]; then
		PERMS=$(stat -c '%a' "$SSH_DIR" 2>/dev/null)
		assert_eq ".ssh dir mode is 700" "700" "$PERMS"
	fi
fi

# ============================================================
# Test 4: Bind mounts (graft)
# ============================================================
log_info "--- Test 4: Bind mounts ---"
if [ -n "$AI_USER" ]; then
	BASHRC_TARGET="$AI_HOME/.bashrc"

	OUT=$(run_as_user list)
	log_info "list output: $OUT"
	assert_contains "list shows ai-user" "$OUT" "$AI_USER"

	if mountpoint -q "$BASHRC_TARGET" 2>/dev/null; then
		log_pass ".bashrc is a mount point"
	else
		log_info ".bashrc not mounted (config may have empty mount.paths)"
	fi
fi

# ============================================================
# Test 5: Permission management
# ============================================================
log_info "--- Test 5: Permission management ---"
if [ -n "$AI_USER" ]; then
	PROJECT_GROUP=$(stat -c '%G' "$PROJECT_DIR" 2>/dev/null)
	log_info "Project dir group: $PROJECT_GROUP"
	assert_eq "project dir owned by ai-user group" "$AI_USER" "$PROJECT_GROUP"

	PROJECT_PERMS=$(stat -c '%a' "$PROJECT_DIR" 2>/dev/null)
	log_info "Project dir permissions: $PROJECT_PERMS"
	GROUP_PERM="${PROJECT_PERMS:1:1}"
	if [ "$GROUP_PERM" -ge 6 ] 2>/dev/null; then
		log_pass "project dir has group write permission"
	else
		log_fail "project dir missing group write (perms=$PROJECT_PERMS)"
	fi

	if [ -g "$PROJECT_DIR" ]; then
		log_pass "SGID bit set on project dir"
	else
		log_fail "SGID bit not set on project dir"
	fi
fi

# ============================================================
# Test 6: mkdir command
# ============================================================
log_info "--- Test 6: mkdir command ---"
if [ -n "$AI_USER" ]; then
	EXTRA_DIR="/tmp/test-extra-dir"
	mkdir -p "$EXTRA_DIR"
	chown "$MAIN_USER:$MAIN_USER" "$EXTRA_DIR"

	OUT=$(run_as_user mkdir "$EXTRA_DIR" "$AI_USER")
	log_info "mkdir output: $OUT"

	assert_dir_exists "mkdir dir exists" "$EXTRA_DIR"
	EXTRA_GROUP=$(stat -c '%G' "$EXTRA_DIR" 2>/dev/null)
	assert_eq "mkdir dir owned by ai-user group" "$AI_USER" "$EXTRA_GROUP"

	if [ -g "$EXTRA_DIR" ]; then
		log_pass "SGID bit set on mkdir dir"
	else
		log_fail "SGID bit not set on mkdir dir"
	fi
fi

# ============================================================
# Test 7: cd command
# ============================================================
log_info "--- Test 7: cd command ---"
if [ -n "$AI_USER" ]; then
	OUT=$(SUDO_USER="$MAIN_USER" "$AI_MIRROR" cd "$PROJECT_DIR" 2>&1) || true
	log_info "cd ai-user dir: $OUT"
	assert_contains "cd outputs cd command" "$OUT" "cd"

	OUT=$(SUDO_USER="$MAIN_USER" "$AI_MIRROR" cd "/home/$AI_USER" 2>&1) || true
	log_info "cd ai-user home: $OUT"
	assert_contains "cd detects ai-user home ownership" "$OUT" "$AI_USER"

	OUT=$(SUDO_USER="$MAIN_USER" "$AI_MIRROR" cd "/home/$MAIN_USER" 2>&1) || true
	log_info "cd main user dir: $OUT"
	assert_contains "cd returns cd for main user" "$OUT" "cd"
fi

# ============================================================
# Test 8: health command
# ============================================================
log_info "--- Test 8: health command ---"
if [ -n "$AI_USER" ]; then
	OUT=$(run_as_user health) || true
	log_info "health output: $OUT"
fi

# ============================================================
# Test 9: Create second project
# ============================================================
log_info "--- Test 9: create second project ---"
OUT2=$(run_as_user create "$PROJECT_DIR2")
AI_USER2=$(echo "$OUT2" | tail -1 | tr -d '[:space:]')
log_info "Created second ai-user: '$AI_USER2'"

if [ -n "$AI_USER2" ] && [ "$AI_USER2" != "$AI_USER" ]; then
	assert_user_exists "second ai-user exists" "$AI_USER2"
	log_pass "second ai-user is different from first"
else
	log_fail "second ai-user creation issue"
fi

# ============================================================
# Test 10: rm command
# ============================================================
log_info "--- Test 10: rm command ---"
if [ -n "$AI_USER2" ]; then
	OUT=$(run_as_user rm "$PROJECT_DIR2") || true
	log_info "rm output: $OUT"

	assert_user_not_exists "ai-user removed after rm" "$AI_USER2"
	assert_dir_exists "project dir preserved after rm" "$PROJECT_DIR2"
fi

# ============================================================
# Test 11: force-destroy command
# ============================================================
log_info "--- Test 11: force-destroy command ---"
if [ -n "$AI_USER" ]; then
	OUT=$(run_as_user force-destroy "$AI_USER") || true
	log_info "force-destroy output: $OUT"

	assert_user_not_exists "ai-user removed after force-destroy" "$AI_USER"
	if [ -d "/home/$AI_USER" ]; then
		log_fail "ai-user home still exists after force-destroy"
	else
		log_pass "ai-user home removed after force-destroy"
	fi
fi

# ============================================================
# Summary
# ============================================================
echo ""
echo "========================================="
echo -e "  ${GREEN}PASS: $PASS${NC}  ${RED}FAIL: $FAIL${NC}"
echo "========================================="
if [ -n "$ERRORS" ]; then
	echo -e "Errors:${RED}$ERRORS${NC}"
fi
echo ""

[ "$FAIL" -gt 0 ] && exit 1
exit 0
