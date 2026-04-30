#!/usr/bin/env bash
#
# ai-mirror create+cd comprehensive test suite
# Tests: SSH key scenarios, permission scenarios, config scenarios, flow scenarios
#
set -euo pipefail

PASS=0
FAIL=0
CURRENT_TEST=""

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log_pass() {
	PASS=$((PASS + 1))
	echo -e "  ${GREEN}[PASS]${NC} $1"
}
log_fail() {
	FAIL=$((FAIL + 1))
	echo -e "  ${RED}[FAIL]${NC} $1"
	# Auto-generate issue file for failed tests
	local ts
	ts=$(date +%Y-%m-%d)
	local safe_name
	safe_name=$(echo "$CURRENT_TEST: $1" | tr '[:upper:]' '[:lower:]' | sed 's/[^a-z0-9]/-/g' | sed 's/--*/-/g' | sed 's/^-//;s/-$//')
	local issue_file="/build/issues/${ts}-${safe_name}.md"
	mkdir -p /build/issues
	cat >"$issue_file" <<ISSUE_EOF
# Issue: $CURRENT_TEST - $1

**Date**: ${ts}
**Source**: test_create_cd_all.sh auto-generated

## Description

Test case \`$CURRENT_TEST\` failed with:
$1

## Context

- Test user: $TEST_USER
- Full test output available in test run logs
ISSUE_EOF
	echo -e "  ${YELLOW}[ISSUE]${NC} Generated: $issue_file"
}
log_info() { echo -e "  ${CYAN}[INFO]${NC} $1"; }
log_warn() { echo -e "  ${YELLOW}[WARN]${NC} $1"; }

begin_test() {
	CURRENT_TEST="$1"
	echo ""
	echo -e "${YELLOW}=== $1 ===${NC}"
}

# Run am as root with SUDO_USER=testuser (requires SUDO_UID for group check)
run_am() { SUDO_USER="$TEST_USER" SUDO_UID=$(id -u "$TEST_USER") HOME="/home/$TEST_USER" /usr/local/bin/am "$@" 2>&1; }

# Start sshd for login test
start_sshd() {
	mkdir -p /run/sshd
	ssh-keygen -A 2>/dev/null || true
	cat >/etc/ssh/sshd_config <<'SSHD'
Port 22
HostKey /etc/ssh/ssh_host_ed25519_key
PubkeyAuthentication yes
PasswordAuthentication no
AuthorizedKeysFile .ssh/authorized_keys
StrictModes yes
SSHD
	/usr/sbin/sshd 2>/dev/null || true
	sleep 0.5
}

stop_sshd() {
	pkill sshd 2>/dev/null || true
	sleep 0.3
}

# Test SSH login: run as TEST_USER, use key_path to connect to ai_user@localhost
test_ssh_login() {
	local ai_user="$1"
	local key_path="$2"
	su - "$TEST_USER" -c "ssh -i $key_path -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o BatchMode=yes -o ConnectTimeout=5 -o IdentitiesOnly=yes ${ai_user}@localhost echo SSH_OK" 2>&1
}

# Cleanup all test state
full_cleanup() {
	stop_sshd
	# Unmount any bind mounts under test home FIRST
	# Must happen before userdel -r which tries to remove the directory tree
	# Use multiple attempts with delays for lazy unmounts to complete
	for attempt in 1 2 3; do
		if [[ -d "/home/$TEST_USER" ]]; then
			# Collect mount points in reverse order (deepest first)
			local mounts=()
			while IFS= read -r line; do
				local mp=$(echo "$line" | awk '{print $2}')
				if [[ "$mp" == /home/$TEST_USER/* ]]; then
					mounts+=("$mp")
				fi
			done < <(grep " /home/$TEST_USER" /proc/mounts 2>/dev/null || true)
			# Unmount in reverse order (deepest first)
			for ((i = ${#mounts[@]} - 1; i >= 0; i--)); do
				umount -l "${mounts[$i]}" 2>/dev/null || true
			done
			# Also unmount any ai-user homes (match any prefix pattern)
			while IFS= read -r line; do
				local mp=$(echo "$line" | awk '{print $2}')
				case "$mp" in
				/home/i${TEST_USER}_* | /home/ai_${TEST_USER}_* | /home/${TEST_USER}_*)
					umount -l "$mp" 2>/dev/null || true
					;;
				esac
			done < <(grep " /home/" /proc/mounts 2>/dev/null || true)
		fi
		sleep 0.3
	done
	# Remove all test ai-users (without -r to avoid deleting home dirs with stale mounts)
	if id "$TEST_USER" &>/dev/null || [[ -d /home/$TEST_USER ]]; then
		# Match any prefix pattern (i, ai_, custom)
		while IFS= read -r line; do
			local uname
			uname=$(echo "$line" | cut -d: -f1)
			case "$uname" in
			i${TEST_USER}_* | ai_${TEST_USER}_* | ${TEST_USER}_*)
				userdel "$uname" 2>/dev/null || true
				groupdel "$uname" 2>/dev/null || true
				;;
			esac
		done </etc/passwd
		userdel "$TEST_USER" 2>/dev/null || true
	fi
	# Final cleanup of directories (use --one-file-system to avoid bind mount issues)
	rm -rf --one-file-system /home/$TEST_USER 2>/dev/null || true
	# For ai-user homes, may have nested bind mounts - use separate rm for each
	for d in /home/i${TEST_USER}_* /home/ai_${TEST_USER}_* /home/${TEST_USER}_*; do
		[[ -d "$d" ]] && rm -rf --one-file-system "$d" 2>/dev/null || true
	done
	rm -rf /tmp/test-project-* 2>/dev/null || true
}

# Create test user with optional SSH setup
# Usage: setup_test_user [none|standard|custom:<path>]
setup_test_user() {
	local key_setup="${1:-none}"
	full_cleanup

	# Force create test user (may fail if partially cleaned, retry)
	useradd -m -s /bin/bash "$TEST_USER" 2>/dev/null ||
		{
			userdel "$TEST_USER" 2>/dev/null
			groupdel "$TEST_USER" 2>/dev/null
			rm -rf /home/$TEST_USER
			useradd -m -s /bin/bash "$TEST_USER"
		}

	# Verify user exists before continuing
	id "$TEST_USER" &>/dev/null || {
		log_fail "setup_test_user: cannot create $TEST_USER"
		return 1
	}

	usermod -aG ai-mirror "$TEST_USER" 2>/dev/null || true
	mkdir -p "/home/$TEST_USER/projects/testproj"
	# chown may fail on bind-mounted files (read-only), ignore errors
	chown -R "$TEST_USER:$TEST_USER" "/home/$TEST_USER" 2>/dev/null || true

	case "$key_setup" in
	none)
		# No keys at all
		;;
	standard)
		mkdir -p "/home/$TEST_USER/.ssh"
		ssh-keygen -t ed25519 -f "/home/$TEST_USER/.ssh/id_ed25519" -N "" -q -C "test"
		chmod 700 "/home/$TEST_USER/.ssh"
		chmod 600 "/home/$TEST_USER/.ssh/id_ed25519"
		chown -R "$TEST_USER:$TEST_USER" "/home/$TEST_USER/.ssh"
		;;
	custom:*)
		local custom_path="${key_setup#custom:}"
		mkdir -p "$(dirname "$custom_path")"
		ssh-keygen -t ed25519 -f "$custom_path" -N "" -q -C "custom"
		chmod 600 "$custom_path"
		chown -R "$TEST_USER:$TEST_USER" "/home/$TEST_USER" 2>/dev/null || true
		;;
	esac
}

# Write config file
write_config() {
	local content="$1"
	echo "$content" >"/home/$TEST_USER/.ai-mirror.toml"
	chown "$TEST_USER:$TEST_USER" "/home/$TEST_USER/.ai-mirror.toml"
}

# ============================================================
TEST_USER="testx"
AM="/usr/local/bin/am"

echo ""
echo "========================================================"
echo "  ai-mirror create+cd Comprehensive Test Suite"
echo "========================================================"

# ============================================================
# S1: No SSH key -> auto-generate ai-mirror key
# ============================================================
begin_test "S1: User has NO SSH key"
setup_test_user none

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')
log_info "create output: $OUT"
log_info "AI_USER=$AI_USER"

if [[ -z "$AI_USER" ]]; then
	log_fail "create returned empty username"
else
	id "$AI_USER" &>/dev/null && log_pass "ai-user created" || log_fail "ai-user not found"

	# Check auto-generated key
	if [[ -f "/home/$TEST_USER/.ssh/ai-mirror" ]]; then
		log_pass "ai-mirror key auto-generated"
	else
		log_fail "ai-mirror key NOT generated"
	fi

	# Check authorized_keys
	AI_HOME=$(getent passwd "$AI_USER" | cut -d: -f6)
	if [[ -f "$AI_HOME/.ssh/authorized_keys" ]]; then
		log_pass "authorized_keys exists"
		AK_OWNER=$(stat -c '%U' "$AI_HOME/.ssh/authorized_keys")
		[[ "$AK_OWNER" == "$AI_USER" ]] && log_pass "authorized_keys owner=$AI_USER" || log_fail "authorized_keys owner=$AK_OWNER (expected $AI_USER)"
		AK_PERMS=$(stat -c '%a' "$AI_HOME/.ssh/authorized_keys")
		[[ "$AK_PERMS" == "600" ]] && log_pass "authorized_keys perms=600" || log_fail "authorized_keys perms=$AK_PERMS (expected 600)"
	else
		log_fail "authorized_keys missing"
	fi

	# SSH login test
	start_sshd
	SSH_OUT=$(test_ssh_login "$AI_USER" "/home/$TEST_USER/.ssh/ai-mirror" 2>&1)
	if echo "$SSH_OUT" | grep -q "SSH_OK"; then
		log_pass "SSH login works"
	else
		log_fail "SSH login failed: $SSH_OUT"
		# Debug: check sshd log
		log_info "authorized_keys content:"
		cat "$AI_HOME/.ssh/authorized_keys" 2>/dev/null || echo "(missing)"
		log_info ".ssh dir:"
		ls -la "$AI_HOME/.ssh/" 2>/dev/null || echo "(missing)"
		log_info "key pub:"
		cat "/home/$TEST_USER/.ssh/ai-mirror.pub" 2>/dev/null || echo "(missing)"
	fi
	stop_sshd

	# CD test
	CD_OUT=$(run_am cd "/home/$TEST_USER/projects/testproj" 2>&1)
	log_info "cd output: $CD_OUT"
	echo "$CD_OUT" | grep -q "action=ssh" && log_pass "cd returns action=ssh" || log_fail "cd does not return action=ssh"
	echo "$CD_OUT" | grep -q "user=$AI_USER" && log_pass "cd returns correct user" || log_fail "cd returns wrong user"
fi

# ============================================================
# S2: User has standard id_ed25519 key
# ============================================================
begin_test "S2: User has standard id_ed25519 key"
setup_test_user standard

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')
log_info "create output (last 3 lines): $(echo "$OUT" | tail -3)"

if [[ -n "$AI_USER" ]] && id "$AI_USER" &>/dev/null; then
	log_pass "ai-user created: $AI_USER"

	AI_HOME=$(getent passwd "$AI_USER" | cut -d: -f6)

	# Check that id_ed25519.pub is in authorized_keys
	if [[ -f "/home/$TEST_USER/.ssh/id_ed25519.pub" ]] && [[ -f "$AI_HOME/.ssh/authorized_keys" ]]; then
		PUB=$(cat "/home/$TEST_USER/.ssh/id_ed25519.pub" | awk '{print $2}')
		if grep -q "$PUB" "$AI_HOME/.ssh/authorized_keys" 2>/dev/null; then
			log_pass "id_ed25519.pub found in authorized_keys"
		else
			log_fail "id_ed25519.pub NOT in authorized_keys"
			log_info "authorized_keys: $(cat $AI_HOME/.ssh/authorized_keys)"
		fi
	fi

	# SSH login with id_ed25519
	start_sshd
	SSH_OUT=$(test_ssh_login "$AI_USER" "/home/$TEST_USER/.ssh/id_ed25519" 2>&1)
	if echo "$SSH_OUT" | grep -q "SSH_OK"; then
		log_pass "SSH login with id_ed25519 works"
	else
		log_fail "SSH login with id_ed25519 failed: $SSH_OUT"
	fi
	stop_sshd
fi

# ============================================================
# S3: Configured key_path does not exist
# ============================================================
begin_test "S3: Configured key_path does not exist"
setup_test_user none
write_config '[user]
prefix = "i"

[ssh]
key_path = "/home/'"$TEST_USER"'/.ssh/my-custom-key"
key_type = "ed25519"

[mount]
paths = []'

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')
log_info "create output (last 5 lines): $(echo "$OUT" | tail -5)"

if [[ -n "$AI_USER" ]] && id "$AI_USER" &>/dev/null; then
	log_pass "ai-user created: $AI_USER"
	if [[ -f "/home/$TEST_USER/.ssh/my-custom-key" ]]; then
		log_pass "key_path was auto-generated"
	else
		log_fail "key_path was NOT auto-generated"
	fi

	AI_HOME=$(getent passwd "$AI_USER" | cut -d: -f6)
	start_sshd
	SSH_OUT=$(test_ssh_login "$AI_USER" "/home/$TEST_USER/.ssh/my-custom-key" 2>&1)
	if echo "$SSH_OUT" | grep -q "SSH_OK"; then
		log_pass "SSH login with custom key works"
	else
		log_fail "SSH login with custom key failed: $SSH_OUT"
	fi
	stop_sshd
fi

# ============================================================
# S4: ai_default_key does not exist
# ============================================================
begin_test "S4: ai_default_key does not exist"
setup_test_user standard
write_config '[user]
prefix = "i"

[ssh]
key_path = "/home/'"$TEST_USER"'/.ssh/ai-mirror"
key_type = "ed25519"
ai_default_key = "/home/'"$TEST_USER"'/.ssh/nonexistent_key"

[mount]
paths = []'

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')
log_info "create output (last 5 lines): $(echo "$OUT" | tail -5)"

if [[ -n "$AI_USER" ]] && id "$AI_USER" &>/dev/null; then
	log_pass "ai-user created: $AI_USER (nonexistent default_key should warn, not block)"
	# Should still work with key_path key
	start_sshd
	SSH_OUT=$(test_ssh_login "$AI_USER" "/home/$TEST_USER/.ssh/ai-mirror" 2>&1)
	if echo "$SSH_OUT" | grep -q "SSH_OK"; then
		log_pass "SSH login still works (fallback to key_path)"
	else
		log_fail "SSH login failed: $SSH_OUT"
	fi
	stop_sshd
fi

# ============================================================
# S5: ai_default_key == user's standard key (DANGEROUS)
# ============================================================
begin_test "S5: ai_default_key == user's id_ed25519 (SECURITY)"
setup_test_user standard
write_config '[user]
prefix = "i"

[ssh]
key_path = "/home/'"$TEST_USER"'/.ssh/ai-mirror"
key_type = "ed25519"
ai_default_key = "/home/'"$TEST_USER"'/.ssh/id_ed25519"

[mount]
paths = []'

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')
log_info "create output (last 5 lines): $(echo "$OUT" | tail -5)"

if [[ -n "$AI_USER" ]] && id "$AI_USER" &>/dev/null; then
	AI_HOME=$(getent passwd "$AI_USER" | cut -d: -f6)
	log_pass "ai-user created: $AI_USER"

	# Check: how many keys in authorized_keys?
	KEY_COUNT=$(wc -l <"$AI_HOME/.ssh/authorized_keys" 2>/dev/null || echo "0")
	log_info "authorized_keys has $KEY_COUNT line(s)"

	# Check if both keys are present (duplicated)
	if [[ -f "/home/$TEST_USER/.ssh/ai-mirror.pub" ]] && [[ -f "/home/$TEST_USER/.ssh/id_ed25519.pub" ]]; then
		PUB1=$(cat "/home/$TEST_USER/.ssh/ai-mirror.pub" | awk '{print $2}')
		PUB2=$(cat "/home/$TEST_USER/.ssh/id_ed25519.pub" | awk '{print $2}')

		if [[ "$PUB1" == "$PUB2" ]]; then
			log_warn "key_path and ai_default_key produce SAME public key (same key pair)"
			# This is ok, only written once due to dedup
			[[ "$KEY_COUNT" -eq 1 ]] && log_pass "deduplicated (1 key in authorized_keys)" || log_warn "multiple entries ($KEY_COUNT)"
		else
			log_info "Two different keys"
			# ai_default_key should NOT be in authorized_keys (security)
			# It should only be installed as ai-user's identity key (~/.ssh/id_ed25519.pub)
			if grep -q "$PUB2" "$AI_HOME/.ssh/authorized_keys" 2>/dev/null; then
				log_fail "SECURITY: ai_default_key pub key is in authorized_keys (should not be)"
			else
				log_pass "ai_default_key NOT in authorized_keys (safe)"
			fi
			# Verify: ai_default_key installed as ai-user's identity key
			if [[ -f "$AI_HOME/.ssh/id_ed25519.pub" ]]; then
				log_pass "ai_default_key installed as ai-user identity key (~/.ssh/id_ed25519.pub)"
			else
				log_warn "ai_default_key not found as identity key in ai-user .ssh"
			fi
		fi
	fi

	# SSH login test
	start_sshd
	SSH_OUT=$(test_ssh_login "$AI_USER" "/home/$TEST_USER/.ssh/ai-mirror" 2>&1)
	if echo "$SSH_OUT" | grep -q "SSH_OK"; then
		log_pass "SSH login with key_path works"
	else
		log_fail "SSH login failed: $SSH_OUT"
	fi
	stop_sshd
fi

# ============================================================
# S6: key_path == id_ed25519 (user's personal key as key_path)
# ============================================================
begin_test "S6: key_path == user's id_ed25519 (auto-detect)"
setup_test_user standard
# No config file -> auto-detect should find id_ed25519
rm -f "/home/$TEST_USER/.ai-mirror.toml"

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')
log_info "create output (last 5 lines): $(echo "$OUT" | tail -5)"

if [[ -n "$AI_USER" ]] && id "$AI_USER" &>/dev/null; then
	log_pass "ai-user created: $AI_USER"

	AI_HOME=$(getent passwd "$AI_USER" | cut -d: -f6)

	# Verify: id_ed25519.pub in authorized_keys
	if [[ -f "/home/$TEST_USER/.ssh/id_ed25519.pub" ]] && [[ -f "$AI_HOME/.ssh/authorized_keys" ]]; then
		PUB=$(cat "/home/$TEST_USER/.ssh/id_ed25519.pub" | awk '{print $2}')
		if grep -q "$PUB" "$AI_HOME/.ssh/authorized_keys" 2>/dev/null; then
			log_pass "auto-detected id_ed25519 is in authorized_keys"
		else
			log_fail "auto-detected id_ed25519 NOT in authorized_keys"
		fi
	fi

	# SSH login with id_ed25519
	start_sshd
	SSH_OUT=$(test_ssh_login "$AI_USER" "/home/$TEST_USER/.ssh/id_ed25519" 2>&1)
	if echo "$SSH_OUT" | grep -q "SSH_OK"; then
		log_pass "SSH login with id_ed25519 works"
	else
		log_fail "SSH login failed: $SSH_OUT"
	fi
	stop_sshd

	# CD test
	CD_OUT=$(run_am cd "/home/$TEST_USER/projects/testproj" 2>&1)
	log_info "cd output: $CD_OUT"
	echo "$CD_OUT" | grep -q "action=ssh" && log_pass "cd returns action=ssh" || log_fail "cd wrong action"
	CD_KEY=$(echo "$CD_OUT" | grep "^key=" | cut -d= -f2)
	log_info "cd key path: $CD_KEY"
	[[ "$CD_KEY" == "/home/$TEST_USER/.ssh/id_ed25519" ]] && log_pass "cd key points to auto-detected id_ed25519" || log_warn "cd key=$CD_KEY (may differ)"
fi

# ============================================================
# S7: key_path is symlink
# ============================================================
begin_test "S7: key_path is a symlink (security)"
setup_test_user standard
ln -sf "/home/$TEST_USER/.ssh/id_ed25519" "/home/$TEST_USER/.ssh/ai-mirror"
chown -h "$TEST_USER:$TEST_USER" "/home/$TEST_USER/.ssh/ai-mirror"

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')
log_info "create output (last 5 lines): $(echo "$OUT" | tail -5)"

if echo "$OUT" | grep -qi "symlink"; then
	log_pass "symlink key rejected with warning"
elif [[ -n "$AI_USER" ]] && id "$AI_USER" &>/dev/null; then
	log_warn "symlink key accepted (may be security issue)"
fi

# ============================================================
# P1: authorized_keys owner is root (after create)
# ============================================================
begin_test "P1: authorized_keys owner wrong -> update fixes"
setup_test_user standard

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

if [[ -n "$AI_USER" ]]; then
	AI_HOME=$(getent passwd "$AI_USER" | cut -d: -f6)

	# Tamper: change owner to root
	chown root:root "$AI_HOME/.ssh/authorized_keys"
	log_info "Tampered authorized_keys owner to root"

	AK_OWNER_BEFORE=$(stat -c '%U' "$AI_HOME/.ssh/authorized_keys")
	log_info "Before update: owner=$AK_OWNER_BEFORE"

	OUT2=$(run_am update "/home/$TEST_USER/projects/testproj" 2>&1)
	log_info "update output (last 3): $(echo "$OUT2" | tail -3)"

	AK_OWNER_AFTER=$(stat -c '%U' "$AI_HOME/.ssh/authorized_keys")
	if [[ "$AK_OWNER_AFTER" == "$AI_USER" ]]; then
		log_pass "update fixed authorized_keys owner to $AI_USER"
	else
		log_fail "update did NOT fix owner (still $AK_OWNER_AFTER)"
	fi
fi

# ============================================================
# P2: .ssh dir owner is root
# ============================================================
begin_test "P2: .ssh dir owner wrong -> update fixes"
setup_test_user standard

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

if [[ -n "$AI_USER" ]]; then
	AI_HOME=$(getent passwd "$AI_USER" | cut -d: -f6)

	chown root:root "$AI_HOME/.ssh"
	log_info "Tampered .ssh owner to root"

	OUT2=$(run_am update "/home/$TEST_USER/projects/testproj" 2>&1)
	log_info "update output (last 3): $(echo "$OUT2" | tail -3)"

	SSH_OWNER=$(stat -c '%U' "$AI_HOME/.ssh")
	if [[ "$SSH_OWNER" == "$AI_USER" ]]; then
		log_pass "update fixed .ssh owner to $AI_USER"
	else
		log_fail "update did NOT fix .ssh owner (still $SSH_OWNER)"
	fi
fi

# ============================================================
# P3: authorized_keys permissions 644
# ============================================================
begin_test "P3: authorized_keys perms 644 -> update fixes"
setup_test_user standard

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

if [[ -n "$AI_USER" ]]; then
	AI_HOME=$(getent passwd "$AI_USER" | cut -d: -f6)
	chmod 644 "$AI_HOME/.ssh/authorized_keys"
	log_info "Tampered authorized_keys to 644"

	run_am update "/home/$TEST_USER/projects/testproj" >/dev/null 2>&1

	PERMS=$(stat -c '%a' "$AI_HOME/.ssh/authorized_keys")
	[[ "$PERMS" == "600" ]] && log_pass "update fixed perms to 600" || log_fail "perms still $PERMS"
fi

# ============================================================
# P4: .ssh dir permissions 755
# ============================================================
begin_test "P4: .ssh perms 755 -> update fixes"
setup_test_user standard

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

if [[ -n "$AI_USER" ]]; then
	AI_HOME=$(getent passwd "$AI_USER" | cut -d: -f6)
	chmod 755 "$AI_HOME/.ssh"
	log_info "Tampered .ssh to 755"

	run_am update "/home/$TEST_USER/projects/testproj" >/dev/null 2>&1

	PERMS=$(stat -c '%a' "$AI_HOME/.ssh")
	[[ "$PERMS" == "700" ]] && log_pass "update fixed .ssh to 700" || log_fail ".ssh perms still $PERMS"
fi

# ============================================================
# C1: No config file -> auto-detect
# ============================================================
begin_test "C1: No config file -> auto-detect defaults"
setup_test_user standard
rm -f "/home/$TEST_USER/.ai-mirror.toml"

OUT=$(run_am config 2>&1)
log_info "config output: $OUT"
echo "$OUT" | grep -q "ed25519" && log_pass "default key_type=ed25519" || log_fail "wrong key_type"
echo "$OUT" | grep -q "prefix" && log_pass "shows prefix" || log_fail "no prefix"

# ============================================================
# C2: Repeated create same project
# ============================================================
begin_test "C2: Repeated create same project"
setup_test_user standard

OUT1=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER1=$(echo "$OUT1" | tail -1 | tr -d '[:space:]')

OUT2=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER2=$(echo "$OUT2" | tail -1 | tr -d '[:space:]')

log_info "First:  $AI_USER1"
log_info "Second: $AI_USER2"

[[ "$AI_USER1" == "$AI_USER2" ]] && log_pass "same username returned" || log_fail "different usernames!"
id "$AI_USER1" &>/dev/null && log_pass "user still exists" || log_fail "user disappeared"

# ============================================================
# C3: Project not under HOME
# ============================================================
begin_test "C3: Project not under HOME (should reject)"
setup_test_user standard
mkdir -p /tmp/evil-project
chown "$TEST_USER:$TEST_USER" /tmp/evil-project

OUT=$(run_am create "/tmp/evil-project" 2>&1 || true)
log_info "create /tmp output: $OUT"
echo "$OUT" | grep -qi "not allowed\|invalid\|error\|failed\|must be under" && log_pass "correctly rejected" || log_fail "did NOT reject out-of-HOME project"

# ============================================================
# D1: CD to project dir
# ============================================================
begin_test "D1: CD to project dir returns correct action"
setup_test_user standard

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

if [[ -n "$AI_USER" ]]; then
	CD_OUT=$(run_am cd "/home/$TEST_USER/projects/testproj" 2>&1)
	log_info "cd output: $CD_OUT"
	echo "$CD_OUT" | grep -q "action=ssh" && log_pass "action=ssh" || log_fail "wrong action"
	echo "$CD_OUT" | grep -q "user=$AI_USER" && log_pass "user correct" || log_fail "user wrong"
	echo "$CD_OUT" | grep -q "key=" && log_pass "key path present" || log_fail "no key path"
fi

# ============================================================
# D2: CD to ai-user home
# ============================================================
begin_test "D2: CD to ai-user home dir"
setup_test_user standard

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

if [[ -n "$AI_USER" ]]; then
	AI_HOME=$(getent passwd "$AI_USER" | cut -d: -f6)
	CD_OUT=$(run_am cd "$AI_HOME" 2>&1)
	log_info "cd ai-home output: $CD_OUT"
	echo "$CD_OUT" | grep -q "action=ssh" && log_pass "action=ssh for ai-home" || log_fail "wrong action"
fi

# ============================================================
# D3: CD to main user home
# ============================================================
begin_test "D3: CD to main user home dir"
setup_test_user standard

CD_OUT=$(run_am cd "/home/$TEST_USER" 2>&1)
log_info "cd main-home output: $CD_OUT"
echo "$CD_OUT" | grep -q "action=cd" && log_pass "action=cd for main-user home" || log_fail "expected action=cd"

# ============================================================
# D4: CD to non-existent path
# ============================================================
begin_test "D4: CD to non-existent path"
setup_test_user standard

CD_OUT=$(run_am cd "/home/$TEST_USER/nonexistent" 2>&1) || true
log_info "cd nonexistent output: $CD_OUT"
echo "$CD_OUT" | grep -qi "not exist\|invalid\|error\|cannot" && log_pass "correctly rejected" || log_fail "did not reject"

# ============================================================
# D5: CD after authorized_keys deleted
# ============================================================
begin_test "D5: CD after authorized_keys deleted (should warn)"
setup_test_user standard

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

if [[ -n "$AI_USER" ]]; then
	AI_HOME=$(getent passwd "$AI_USER" | cut -d: -f6)
	rm -f "$AI_HOME/.ssh/authorized_keys"

	CD_OUT=$(run_am cd "/home/$TEST_USER/projects/testproj" 2>&1) || true
	log_info "cd after delete output: $CD_OUT"
	echo "$CD_OUT" | grep -qi "warning\|missing\|SSH" && log_pass "warning issued for missing SSH" || log_warn "no warning for missing authorized_keys"
fi

# ============================================================
# E1: rm project then create same-name project
# ============================================================
begin_test "E1: rm project then create same-name project"
setup_test_user standard

# Create first project
OUT1=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER1=$(echo "$OUT1" | tail -1 | tr -d '[:space:]')
log_info "First create: $AI_USER1"

if [[ -n "$AI_USER1" ]]; then
	log_pass "first ai-user created: $AI_USER1"

	# Remove the project
	RM_OUT=$(run_am rm "/home/$TEST_USER/projects/testproj" 2>&1)
	log_info "rm output: $RM_OUT"

	if echo "$RM_OUT" | grep -q "Removed:"; then
		log_pass "project removed successfully"

		# Recreate the project directory (rm may have removed it)
		mkdir -p "/home/$TEST_USER/projects/testproj"
		chown "$TEST_USER:$TEST_USER" "/home/$TEST_USER/projects/testproj"

		# Create same-name project again
		OUT2=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
		AI_USER2=$(echo "$OUT2" | tail -1 | tr -d '[:space:]')
		log_info "Second create: $AI_USER2"

		if [[ -n "$AI_USER2" ]] && id "$AI_USER2" &>/dev/null; then
			log_pass "second ai-user created: $AI_USER2"
			[[ "$AI_USER1" == "$AI_USER2" ]] && log_pass "same username" || log_pass "different username (acceptable)"
		else
			log_fail "second create failed: $OUT2"
		fi
	else
		log_fail "rm failed: $RM_OUT"
	fi
else
	log_fail "first create failed: $OUT1"
fi

# ============================================================
# E2: rm project - check system residue
# ============================================================
begin_test "E2: rm project - check system residue"
setup_test_user standard

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

if [[ -n "$AI_USER" ]]; then
	AI_HOME=$(getent passwd "$AI_USER" | cut -d: -f6)
	AI_UID=$(getent passwd "$AI_USER" | cut -d: -f3)
	AI_GID=$(getent passwd "$AI_USER" | cut -d: -f4)
	log_info "Created user: $AI_USER uid=$AI_UID gid=$AI_GID home=$AI_HOME"

	# Remove the project
	RM_OUT=$(run_am rm "/home/$TEST_USER/projects/testproj" 2>&1)
	log_info "rm output: $RM_OUT"

	RESIDUE=0

	# Check /etc/passwd
	if getent passwd "$AI_USER" &>/dev/null; then
		log_fail "user still in /etc/passwd"
		RESIDUE=$((RESIDUE + 1))
	else
		log_pass "user removed from /etc/passwd"
	fi

	# Check /etc/group
	if getent group "$AI_USER" &>/dev/null; then
		log_fail "group still in /etc/group"
		RESIDUE=$((RESIDUE + 1))
	else
		log_pass "group removed from /etc/group"
	fi

	# Check home directory
	if [[ -d "$AI_HOME" ]]; then
		log_fail "home directory still exists: $AI_HOME"
		RESIDUE=$((RESIDUE + 1))
	else
		log_pass "home directory removed"
	fi

	# Check .am_status file
	if [[ -f "/home/$TEST_USER/projects/testproj/.am_status" ]]; then
		log_fail ".am_status file still exists"
		RESIDUE=$((RESIDUE + 1))
	else
		log_pass ".am_status file removed"
	fi

	# Check bind mounts
	if grep -q "$AI_HOME" /proc/mounts 2>/dev/null; then
		log_fail "bind mounts still active"
		RESIDUE=$((RESIDUE + 1))
	else
		log_pass "no stale bind mounts"
	fi

	# Check user in main_user's supplementary groups
	if id "$TEST_USER" 2>/dev/null | grep -q "$AI_USER"; then
		log_warn "test user still has $AI_USER in supplementary groups (may need newgrp)"
	else
		log_pass "no group residue in test user"
	fi

	[[ $RESIDUE -eq 0 ]] && log_pass "no system residue found" || log_fail "$RESIDUE residue item(s) found"
fi

# ============================================================
# E3: rm project - check system integrity
# ============================================================
begin_test "E3: rm project - check system integrity"
setup_test_user standard

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

if [[ -n "$AI_USER" ]]; then
	# Remove the project
	RM_OUT=$(run_am rm "/home/$TEST_USER/projects/testproj" 2>&1)
	log_info "rm output: $RM_OUT"

	# Check system integrity
	SYS_OK=1

	# Check that main user still exists and is intact
	if id "$TEST_USER" &>/dev/null; then
		log_pass "main user $TEST_USER still exists"
	else
		log_fail "main user $TEST_USER was deleted!"
		SYS_OK=0
	fi

	# Check main user home directory
	if [[ -d "/home/$TEST_USER" ]]; then
		log_pass "main user home directory intact"
	else
		log_fail "main user home directory was deleted!"
		SYS_OK=0
	fi

	# Check main user's .ssh directory
	if [[ -d "/home/$TEST_USER/.ssh" ]]; then
		log_pass "main user .ssh directory intact"
	else
		log_warn "main user .ssh directory missing (may be expected)"
	fi

	# Check main user's projects directory
	if [[ -d "/home/$TEST_USER/projects" ]]; then
		log_pass "projects directory intact"
	else
		log_fail "projects directory was deleted!"
		SYS_OK=0
	fi

	# Check that /etc/passwd is not corrupted (spot check)
	if getent passwd root &>/dev/null && getent passwd "$TEST_USER" &>/dev/null; then
		log_pass "/etc/passwd not corrupted"
	else
		log_fail "/etc/passwd appears corrupted!"
		SYS_OK=0
	fi

	# Check that sudo still works (if available)
	if command -v sudo &>/dev/null; then
		log_pass "sudo command still available"
	else
		log_warn "sudo not available (may be expected in container)"
	fi

	[[ $SYS_OK -eq 1 ]] && log_pass "system integrity OK" || log_fail "system integrity compromised"
fi

# ============================================================
# F1: force-destroy by username
# ============================================================
begin_test "F1: force-destroy by username"
setup_test_user standard

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

if [[ -n "$AI_USER" ]]; then
	AI_HOME=$(getent passwd "$AI_USER" | cut -d: -f6)
	log_info "Created: $AI_USER home=$AI_HOME"

	# Check bind mount exists
	if findmnt "$AI_HOME/.bashrc" >/dev/null 2>&1; then
		log_pass ".bashrc is mounted before force-destroy"
	else
		log_warn ".bashrc not mounted (config may differ)"
	fi

	# Force-destroy by username
	FD_OUT=$(run_am force-destroy "$AI_USER" 2>&1)
	log_info "force-destroy output: $FD_OUT"

	# Verify user removed
	if id "$AI_USER" &>/dev/null; then
		log_fail "user still exists after force-destroy"
	else
		log_pass "user removed after force-destroy"
	fi

	# Verify mounts cleaned
	if findmnt "$AI_HOME/.bashrc" >/dev/null 2>&1; then
		log_fail "mount still exists after force-destroy"
	else
		log_pass "mounts cleaned after force-destroy"
	fi

	# Verify home dir NOT removed (force-destroy doesn't rm home)
	if [[ -d "$AI_HOME" ]]; then
		log_pass "home dir preserved after force-destroy (expected)"
	else
		log_warn "home dir removed (unexpected for force-destroy)"
	fi
fi

# ============================================================
# F2: force-destroy by project path
# ============================================================
begin_test "F2: force-destroy by project path"
setup_test_user standard

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

if [[ -n "$AI_USER" ]]; then
	log_info "Created: $AI_USER"

	# Force-destroy by project path (alternative input)
	FD_OUT=$(run_am force-destroy "/home/$TEST_USER/projects/testproj" 2>&1)
	log_info "force-destroy by path output: $FD_OUT"

	if id "$AI_USER" &>/dev/null; then
		log_fail "user still exists"
	else
		log_pass "user removed via project path input"
	fi
fi

# ============================================================
# M1: mkdir success path
# ============================================================
begin_test "M1: mkdir success path"
setup_test_user standard

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

if [[ -n "$AI_USER" ]]; then
	EXTRA_DIR="/home/$TEST_USER/projects/extra-dir"

	# mkdir should create dir and grant write access
	MK_OUT=$(run_am mkdir "$EXTRA_DIR" "$AI_USER" 2>&1)
	log_info "mkdir output: $MK_OUT"

	if [[ -d "$EXTRA_DIR" ]]; then
		log_pass "directory created"
	else
		log_fail "directory not created"
	fi

	# Check group ownership
	DIR_GROUP=$(stat -c '%G' "$EXTRA_DIR" 2>/dev/null)
	if [[ "$DIR_GROUP" == "$AI_USER" ]]; then
		log_pass "group ownership set to ai-user"
	else
		log_fail "wrong group: $DIR_GROUP"
	fi

	# Check SGID
	if [[ -g "$EXTRA_DIR" ]]; then
		log_pass "SGID bit set"
	else
		log_fail "SGID not set"
	fi

	# Check group write
	PERMS=$(stat -c '%a' "$EXTRA_DIR" 2>/dev/null)
	GROUP_PERM="${PERMS:1:1}"
	if [[ "$GROUP_PERM" -ge 6 ]]; then
		log_pass "group has write permission"
	else
		log_fail "group missing write: $PERMS"
	fi
fi

# ============================================================
# M2: mkdir on existing dir
# ============================================================
begin_test "M2: mkdir on existing dir (grant only)"
setup_test_user standard

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

if [[ -n "$AI_USER" ]]; then
	# Pre-create directory with wrong ownership
	EXTRA_DIR="/home/$TEST_USER/projects/preexist-dir"
	mkdir -p "$EXTRA_DIR"
	chown "$TEST_USER:$TEST_USER" "$EXTRA_DIR"
	chmod 755 "$EXTRA_DIR"

	MK_OUT=$(run_am mkdir "$EXTRA_DIR" "$AI_USER" 2>&1)
	log_info "mkdir existing output: $MK_OUT"

	DIR_GROUP=$(stat -c '%G' "$EXTRA_DIR" 2>/dev/null)
	[[ "$DIR_GROUP" == "$AI_USER" ]] && log_pass "ownership fixed" || log_fail "wrong group: $DIR_GROUP"
	[[ -g "$EXTRA_DIR" ]] && log_pass "SGID set" || log_fail "no SGID"
fi

# ============================================================
# M3: touch success path
# ============================================================
begin_test "M3: touch success path"
setup_test_user standard

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

if [[ -n "$AI_USER" ]]; then
	EXTRA_FILE="/home/$TEST_USER/projects/testproj/extra-file.txt"

	# touch should create file with ai-user ownership
	TCH_OUT=$(run_am touch "$EXTRA_FILE" "$AI_USER" 2>&1)
	log_info "touch output: $TCH_OUT"

	if [[ -f "$EXTRA_FILE" ]]; then
		log_pass "file created"
	else
		log_fail "file not created"
	fi

	FILE_OWNER=$(stat -c '%U' "$EXTRA_FILE" 2>/dev/null)
	[[ "$FILE_OWNER" == "$AI_USER" ]] && log_pass "file owned by ai-user" || log_fail "wrong owner: $FILE_OWNER"

	FILE_PERMS=$(stat -c '%a' "$EXTRA_FILE" 2>/dev/null)
	[[ "$FILE_PERMS" == "600" ]] && log_pass "file mode 600" || log_warn "unexpected mode: $FILE_PERMS"
fi

# ============================================================
# M4: touch on existing file
# ============================================================
begin_test "M4: touch on existing file"
setup_test_user standard

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

if [[ -n "$AI_USER" ]]; then
	# Pre-create file with wrong ownership
	EXTRA_FILE="/home/$TEST_USER/projects/testproj/preexist.txt"
	touch "$EXTRA_FILE"
	chown "$TEST_USER:$TEST_USER" "$EXTRA_FILE"
	chmod 644 "$EXTRA_FILE"

	TCH_OUT=$(run_am touch "$EXTRA_FILE" "$AI_USER" 2>&1)
	log_info "touch existing output: $TCH_OUT"

	FILE_OWNER=$(stat -c '%U' "$EXTRA_FILE" 2>/dev/null)
	[[ "$FILE_OWNER" == "$AI_USER" ]] && log_pass "ownership fixed" || log_fail "wrong owner: $FILE_OWNER"
fi

# ============================================================
# U1: update remounts missing bind mounts
# ============================================================
begin_test "U1: update remounts missing bind mounts"
setup_test_user standard

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

if [[ -n "$AI_USER" ]]; then
	AI_HOME=$(getent passwd "$AI_USER" | cut -d: -f6)

	# Verify initial mount
	if findmnt "$AI_HOME/.bashrc" >/dev/null 2>&1; then
		log_pass "initial .bashrc mounted"
	else
		log_warn "initial .bashrc not mounted"
	fi

	# Unmount it manually
	umount -l "$AI_HOME/.bashrc" 2>/dev/null || true
	sleep 0.3

	if ! findmnt "$AI_HOME/.bashrc" >/dev/null 2>&1; then
		log_pass "mount removed manually"
	else
		log_warn "could not unmount (Docker limitation)"
	fi

	# Run update
	UPD_OUT=$(run_am update "/home/$TEST_USER/projects/testproj" 2>&1)
	log_info "update output: $UPD_OUT"

	# Check mount restored
	if findmnt "$AI_HOME/.bashrc" >/dev/null 2>&1; then
		log_pass "mount restored by update"
	else
		log_fail "mount NOT restored by update"
	fi
fi

# ============================================================
# U2: update removes stale mounts
# ============================================================
begin_test "U2: update removes stale mounts"
setup_test_user standard

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

if [[ -n "$AI_USER" ]]; then
	AI_HOME=$(getent passwd "$AI_USER" | cut -d: -f6)

	# Create a stale mount point (not in config)
	STALE_FILE="$AI_HOME/.stale_config"
	touch "/home/$TEST_USER/.stale_config" 2>/dev/null || true
	mkdir -p "$AI_HOME"

	# Attempt bind mount (may fail in Docker without --privileged)
	mount --bind "/home/$TEST_USER/.stale_config" "$STALE_FILE" 2>/dev/null || log_warn "cannot create stale mount (Docker)"

	if findmnt "$STALE_FILE" >/dev/null 2>&1; then
		log_info "stale mount created"

		UPD_OUT=$(run_am update "/home/$TEST_USER/projects/testproj" 2>&1)
		log_info "update output: $UPD_OUT"

		if ! findmnt "$STALE_FILE" >/dev/null 2>&1; then
			log_pass "stale mount removed by update"
		else
			log_fail "stale mount NOT removed"
		fi
	else
		log_warn "stale mount test skipped (Docker limitation)"
	fi
fi

# ============================================================
# G1: create with multiple mount.paths
# ============================================================
begin_test "G1: create with multiple mount.paths"
setup_test_user standard

# Write config with multiple mount paths (chown to test user for security check)
cat >"/home/$TEST_USER/.ai-mirror.toml" <<'CFG'
[user]
prefix = "i"

[mount]
paths = ["~/.bashrc", "~/.config"]

[ssh]
key_type = "ed25519"
CFG
chown "$TEST_USER:$TEST_USER" "/home/$TEST_USER/.ai-mirror.toml"

mkdir -p "/home/$TEST_USER/.config"
echo "test config" >"/home/$TEST_USER/.config/test.conf"
chown -R "$TEST_USER:$TEST_USER" "/home/$TEST_USER/.config"

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

if [[ -n "$AI_USER" ]]; then
	AI_HOME=$(getent passwd "$AI_USER" | cut -d: -f6)

	# Check .bashrc mount
	if findmnt "$AI_HOME/.bashrc" >/dev/null 2>&1; then
		log_pass ".bashrc mounted"
	else
		# Check if mount was attempted (may not show in findmnt in some Docker setups)
		if echo "$OUT" | grep -q "Bind mounted.*\.bashrc"; then
			log_pass ".bashrc mount reported successful"
		else
			log_fail ".bashrc NOT mounted"
		fi
	fi

	# Check .config mount (directory bind mount)
	if findmnt "$AI_HOME/.config" >/dev/null 2>&1; then
		log_pass ".config mounted"
	else
		if echo "$OUT" | grep -q "Bind mounted.*\.config"; then
			log_pass ".config mount reported successful"
		else
			log_fail ".config NOT mounted"
		fi
	fi

	# Verify .config content readable
	if [[ -f "$AI_HOME/.config/test.conf" ]]; then
		grep -q "test config" "$AI_HOME/.config/test.conf" && log_pass ".config content visible" || log_fail ".config content wrong"
	else
		log_fail ".config/test.conf not found"
	fi
fi

# ============================================================
# G2: create with custom prefix
# ============================================================
begin_test "G2: create with custom prefix"

# Aggressive cleanup: remove ALL possible ai-users from any prefix
for uname in $(awk -F: '{print $1}' /etc/passwd); do
	case "$uname" in
	i${TEST_USER}_* | ai_${TEST_USER}_*)
		umount -lR "$(getent passwd "$uname" | cut -d: -f6)" 2>/dev/null || true
		userdel "$uname" 2>/dev/null || true
		groupdel "$uname" 2>/dev/null || true
		;;
	esac
done
userdel "$TEST_USER" 2>/dev/null || true
groupdel "$TEST_USER" 2>/dev/null || true
rm -rf /home/${TEST_USER} /home/i${TEST_USER}_* /home/ai_${TEST_USER}_* 2>/dev/null || true

setup_test_user standard

# Write config with custom prefix (chown to test user for security check)
cat >"/home/$TEST_USER/.ai-mirror.toml" <<'CFG'
[user]
prefix = "ai_"

[mount]
paths = ["~/.bashrc"]

[ssh]
key_type = "ed25519"
CFG
chown "$TEST_USER:$TEST_USER" "/home/$TEST_USER/.ai-mirror.toml"

log_info "Config owner: $(stat -c '%U:%G' /home/$TEST_USER/.ai-mirror.toml)"
log_info "Config content:"
cat "/home/$TEST_USER/.ai-mirror.toml" | while IFS= read -r line; do log_info "  $line"; done

# Verify config loads correctly
CFG_OUT=$(run_am config 2>&1)
log_info "config output: $CFG_OUT"

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

if [[ -n "$AI_USER" ]]; then
	log_info "Created user: $AI_USER"

	# Verify prefix in username
	if [[ "$AI_USER" == ai_${TEST_USER}_* ]]; then
		log_pass "custom prefix 'ai_' in username"
	else
		log_fail "wrong prefix format: $AI_USER"
	fi

	id "$AI_USER" &>/dev/null && log_pass "user exists" || log_fail "user not found"
fi

# ============================================================
# W1: create -> cd -> update -> cd workflow
# ============================================================
begin_test "W1: create -> cd -> update -> cd workflow"
setup_test_user standard

# Step 1: create
OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')
log_info "Step 1: create -> $AI_USER"

if [[ -n "$AI_USER" ]]; then
	# Step 2: cd (should return action=ssh)
	CD1=$(run_am cd "/home/$TEST_USER/projects/testproj" 2>&1)
	echo "$CD1" | grep -q "action=ssh" && log_pass "step2: cd returns ssh" || log_fail "step2: wrong action"
	echo "$CD1" | grep -q "user=$AI_USER" && log_pass "step2: user correct" || log_fail "step2: wrong user"

	# Step 3: update (remount, fix permissions)
	UPD=$(run_am update "/home/$TEST_USER/projects/testproj" 2>&1)
	log_info "step3: update done"

	# Step 4: cd again (should still work)
	CD2=$(run_am cd "/home/$TEST_USER/projects/testproj" 2>&1)
	echo "$CD2" | grep -q "action=ssh" && log_pass "step4: cd still works" || log_fail "step4: cd broken after update"
fi

# ============================================================
# W2: create -> mkdir -> touch -> cd workflow
# ============================================================
begin_test "W2: create -> mkdir -> touch -> cd workflow"
setup_test_user standard

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

if [[ -n "$AI_USER" ]]; then
	# mkdir additional dir
	MK_OUT=$(run_am mkdir "/home/$TEST_USER/projects/data" "$AI_USER" 2>&1)
	[[ -d "/home/$TEST_USER/projects/data" ]] && log_pass "mkdir created data dir" || log_fail "mkdir failed"

	# touch file in new dir
	TCH_OUT=$(run_am touch "/home/$TEST_USER/projects/data/file.txt" "$AI_USER" 2>&1)
	[[ -f "/home/$TEST_USER/projects/data/file.txt" ]] && log_pass "touch created file" || log_fail "touch failed"

	# cd should still work
	CD=$(run_am cd "/home/$TEST_USER/projects/testproj" 2>&1)
	echo "$CD" | grep -q "action=ssh" && log_pass "cd works after mkdir/touch" || log_fail "cd broken"
fi

# ============================================================
# W3: create -> rm -> create same project workflow
# ============================================================
begin_test "W3: create -> rm -> create same project workflow"
setup_test_user standard

# First create
OUT1=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI1=$(echo "$OUT1" | tail -1 | tr -d '[:space:]')
log_info "First create: $AI1"

if [[ -n "$AI1" ]]; then
	# rm
	RM_OUT=$(run_am rm "/home/$TEST_USER/projects/testproj" 2>&1)
	log_info "rm done"

	# Verify removed
	if id "$AI1" &>/dev/null; then
		log_fail "first user still exists after rm"
	else
		log_pass "first user removed"
	fi

	# Recreate
	mkdir -p "/home/$TEST_USER/projects/testproj"
	chown "$TEST_USER:$TEST_USER" "/home/$TEST_USER/projects/testproj"

	OUT2=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
	AI2=$(echo "$OUT2" | tail -1 | tr -d '[:space:]')
	log_info "Second create: $AI2"

	if [[ -n "$AI2" ]]; then
		[[ "$AI1" == "$AI2" ]] && log_pass "same username on recreate" || log_pass "new username on recreate"
		id "$AI2" &>/dev/null && log_pass "second user exists" || log_fail "second create failed"
	fi
fi

# ============================================================
# Summary
# ============================================================
full_cleanup

echo ""
echo "========================================================"
echo -e "  ${GREEN}PASS: $PASS${NC}  ${RED}FAIL: $FAIL${NC}"
echo "========================================================"

[[ $FAIL -gt 0 ]] && exit 1
exit 0
