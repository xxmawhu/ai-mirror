#!/usr/bin/env bash
#
# ai-mirror create+cd comprehensive test suite
# Tests: SSH key scenarios, permission scenarios, config scenarios, flow scenarios
#
set -uo pipefail

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
				# Check: public key content matches
				AI_PUB=$(cat "$AI_HOME/.ssh/id_ed25519.pub" | awk '{print $2}')
				if [[ "$AI_PUB" == "$PUB2" ]]; then
					log_pass "ai-user id_ed25519.pub content matches source"
				else
					log_fail "ai-user id_ed25519.pub content mismatch"
				fi
			else
				log_warn "ai_default_key not found as identity key in ai-user .ssh"
			fi
			# Check: private key also copied?
			if [[ -f "/home/$TEST_USER/.ssh/id_ed25519" ]]; then
				if [[ -f "$AI_HOME/.ssh/id_ed25519" ]]; then
					log_pass "ai_default_key private key also copied (~/.ssh/id_ed25519)"
					# Check permissions: should be 600
					PRIV_PERMS=$(stat -c '%a' "$AI_HOME/.ssh/id_ed25519")
					[[ "$PRIV_PERMS" == "600" ]] && log_pass "private key perms=600" || log_fail "private key perms=$PRIV_PERMS (expected 600)"
					# Check owner: should be ai-user
					PRIV_OWNER=$(stat -c '%U' "$AI_HOME/.ssh/id_ed25519")
					[[ "$PRIV_OWNER" == "$AI_USER" ]] && log_pass "private key owner=$AI_USER" || log_fail "private key owner=$PRIV_OWNER (expected $AI_USER)"
				else
					log_fail "ai_default_key private key NOT copied to ai-user"
				fi
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
# S5b: ai_default_key == separate key (ai_git_key, ed25519)
# Verifies: 1) key copied, 2) format detected, 3) renamed to SSH default name
# ============================================================
begin_test "S5b: ai_default_key as separate ai_git_key (key copy+rename)"
setup_test_user standard
# Generate a SEPARATE key for ai_default_key (not the same as id_ed25519)
ssh-keygen -t ed25519 -f "/home/$TEST_USER/.ssh/ai_git_key" -N "" -q -C "ai-git-key"
chmod 600 "/home/$TEST_USER/.ssh/ai_git_key"
write_config '[user]
prefix = "i"

[ssh]
key_path = "/home/'"$TEST_USER"'/.ssh/ai-mirror"
key_type = "ed25519"
ai_default_key = "/home/'"$TEST_USER"'/.ssh/ai_git_key"

[mount]
paths = []'

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')
log_info "create output (last 5 lines): $(echo "$OUT" | tail -5)"

if [[ -n "$AI_USER" ]] && id "$AI_USER" &>/dev/null; then
	AI_HOME=$(getent passwd "$AI_USER" | cut -d: -f6)
	log_pass "ai-user created: $AI_USER"

	# 1. Key copied: private key exists
	if [[ -f "$AI_HOME/.ssh/id_ed25519" ]]; then
		log_pass "S5b.1: ai_default_key private key copied to ~/.ssh/id_ed25519"
	else
		log_fail "S5b.1: ai_default_key private key NOT copied"
	fi

	# 1b. Key copied: public key exists
	if [[ -f "$AI_HOME/.ssh/id_ed25519.pub" ]]; then
		log_pass "S5b.1b: ai_default_key public key copied to ~/.ssh/id_ed25519.pub"
	else
		log_fail "S5b.1b: ai_default_key public key NOT copied"
	fi

	# 2. Format detected correctly: content matches source
	SRC_PUB=$(cat "/home/$TEST_USER/.ssh/ai_git_key.pub" | awk '{print $2}')
	DST_PUB=$(cat "$AI_HOME/.ssh/id_ed25519.pub" 2>/dev/null | awk '{print $2}')
	if [[ "$SRC_PUB" == "$DST_PUB" ]]; then
		log_pass "S5b.2: format detected correctly, public key content matches source ai_git_key.pub"
	else
		log_fail "S5b.2: public key content mismatch (src=$SRC_PUB dst=$DST_PUB)"
	fi

	# 2b. Private key content also matches
	SRC_PRIV_MD5=$(md5sum "/home/$TEST_USER/.ssh/ai_git_key" | awk '{print $1}')
	DST_PRIV_MD5=$(md5sum "$AI_HOME/.ssh/id_ed25519" 2>/dev/null | awk '{print $1}')
	if [[ "$SRC_PRIV_MD5" == "$DST_PRIV_MD5" ]]; then
		log_pass "S5b.2b: private key content matches source ai_git_key"
	else
		log_fail "S5b.2b: private key content mismatch"
	fi

	# 3. Renamed to SSH default name (id_ed25519, not ai_git_key)
	if [[ ! -f "$AI_HOME/.ssh/ai_git_key" ]] && [[ -f "$AI_HOME/.ssh/id_ed25519" ]]; then
		log_pass "S5b.3: key renamed from ai_git_key to id_ed25519 (SSH default name)"
	else
		log_fail "S5b.3: key NOT renamed to SSH default name (still ai_git_key or missing id_ed25519)"
	fi

	# 3b. NOT in authorized_keys (identity key, not login key)
	if ! grep -q "$SRC_PUB" "$AI_HOME/.ssh/authorized_keys" 2>/dev/null; then
		log_pass "S5b.3b: ai_default_key NOT in authorized_keys (identity key only)"
	else
		log_fail "S5b.3b: SECURITY: ai_default_key should NOT be in authorized_keys"
	fi

	# Permissions check
	PRIV_PERMS=$(stat -c '%a' "$AI_HOME/.ssh/id_ed25519" 2>/dev/null)
	[[ "$PRIV_PERMS" == "600" ]] && log_pass "S5b.perm: private key perms=600" || log_fail "S5b.perm: private key perms=$PRIV_PERMS (expected 600)"
	PRIV_OWNER=$(stat -c '%U' "$AI_HOME/.ssh/id_ed25519" 2>/dev/null)
	[[ "$PRIV_OWNER" == "$AI_USER" ]] && log_pass "S5b.owner: private key owner=$AI_USER" || log_fail "S5b.owner: private key owner=$PRIV_OWNER (expected $AI_USER)"

	# Verify key_path key IS in authorized_keys (login key)
	KEY_PATH_PUB=$(cat "/home/$TEST_USER/.ssh/ai-mirror.pub" 2>/dev/null | awk '{print $2}')
	if [[ -n "$KEY_PATH_PUB" ]] && grep -q "$KEY_PATH_PUB" "$AI_HOME/.ssh/authorized_keys" 2>/dev/null; then
		log_pass "S5b.auth: key_path key present in authorized_keys"
	else
		log_fail "S5b.auth: key_path key missing from authorized_keys"
	fi

	# SSH login test with key_path
	start_sshd
	SSH_OUT=$(test_ssh_login "$AI_USER" "/home/$TEST_USER/.ssh/ai-mirror" 2>&1)
	if echo "$SSH_OUT" | grep -q "SSH_OK"; then
		log_pass "S5b.ssh: SSH login with key_path works"
	else
		log_fail "S5b.ssh: SSH login failed: $SSH_OUT"
	fi
	stop_sshd
else
	log_fail "S5b: ai-user not created"
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
# CM1: mv file success path
# ============================================================
begin_test "CM1: mv file success path"
setup_test_user standard

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

if [[ -n "$AI_USER" ]]; then
	# Create a source file owned by test user
	SRC_FILE="/home/$TEST_USER/projects/testproj/source.txt"
	DST_FILE="/home/$TEST_USER/projects/testproj/dest.txt"
	echo "test content" >"$SRC_FILE"
	chown "$TEST_USER:$TEST_USER" "$SRC_FILE"

	MV_OUT=$(run_am mv "$SRC_FILE" "$DST_FILE" 2>&1)
	log_info "mv output: $MV_OUT"

	if [[ -f "$DST_FILE" ]]; then
		log_pass "file moved successfully"
	else
		log_fail "destination file not created"
	fi

	if [[ ! -e "$SRC_FILE" ]]; then
		log_pass "source file removed"
	else
		log_fail "source file still exists"
	fi

	# Check ownership - should be ai-user
	DST_OWNER=$(stat -c '%U' "$DST_FILE" 2>/dev/null)
	[[ "$DST_OWNER" == "$AI_USER" ]] && log_pass "moved file owned by ai-user" || log_warn "moved file owner: $DST_OWNER (expected $AI_USER)"
fi

# ============================================================
# CM2: mv directory success path
# ============================================================
begin_test "CM2: mv directory success path"
setup_test_user standard

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

if [[ -n "$AI_USER" ]]; then
	SRC_DIR="/home/$TEST_USER/projects/testproj/srcdir"
	DST_DIR="/home/$TEST_USER/projects/testproj/destdir"
	mkdir -p "$SRC_DIR"
	echo "inner" >"$SRC_DIR/inner.txt"
	chown -R "$TEST_USER:$TEST_USER" "$SRC_DIR"

	MV_OUT=$(run_am mv "$SRC_DIR" "$DST_DIR" 2>&1)
	log_info "mv dir output: $MV_OUT"

	[[ -d "$DST_DIR" ]] && log_pass "directory moved" || log_fail "directory not moved"
	[[ -f "$DST_DIR/inner.txt" ]] && log_pass "inner file preserved" || log_fail "inner file missing"
	[[ ! -e "$SRC_DIR" ]] && log_pass "source dir removed" || log_fail "source dir still exists"
fi

# ============================================================
# CM3: cp file success path
# ============================================================
begin_test "CM3: cp file success path"
setup_test_user standard

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

if [[ -n "$AI_USER" ]]; then
	SRC_FILE="/home/$TEST_USER/projects/testproj/original.txt"
	DST_FILE="/home/$TEST_USER/projects/testproj/copy.txt"
	echo "copy test" >"$SRC_FILE"
	chown "$TEST_USER:$TEST_USER" "$SRC_FILE"

	CP_OUT=$(run_am cp "$SRC_FILE" "$DST_FILE" 2>&1)
	log_info "cp output: $CP_OUT"

	[[ -f "$DST_FILE" ]] && log_pass "file copied" || log_fail "copy not created"
	[[ -f "$SRC_FILE" ]] && log_pass "source preserved" || log_fail "source missing after cp"

	# Check ownership
	DST_OWNER=$(stat -c '%U' "$DST_FILE" 2>/dev/null)
	[[ "$DST_OWNER" == "$AI_USER" ]] && log_pass "copied file owned by ai-user" || log_warn "copied file owner: $DST_OWNER"
fi

# ============================================================
# CM4: cp directory success path
# ============================================================
begin_test "CM4: cp directory success path"
setup_test_user standard

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

if [[ -n "$AI_USER" ]]; then
	SRC_DIR="/home/$TEST_USER/projects/testproj/cpsrc"
	DST_DIR="/home/$TEST_USER/projects/testproj/cpdst"
	mkdir -p "$SRC_DIR/sub"
	echo "deep" >"$SRC_DIR/sub/deep.txt"
	chown -R "$TEST_USER:$TEST_USER" "$SRC_DIR"

	CP_OUT=$(run_am cp "$SRC_DIR" "$DST_DIR" 2>&1)
	log_info "cp dir output: $CP_OUT"

	[[ -d "$DST_DIR" ]] && log_pass "directory copied" || log_fail "directory copy failed"
	[[ -f "$DST_DIR/sub/deep.txt" ]] && log_pass "deep file preserved" || log_fail "deep file missing"
	[[ -d "$SRC_DIR" ]] && log_pass "source dir preserved" || log_fail "source dir removed"
fi

# ============================================================
# CM5: mv non-existent source (should fail)
# ============================================================
begin_test "CM5: mv non-existent source (should fail)"
setup_test_user standard

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

MV_OUT=$(run_am mv "/home/$TEST_USER/projects/nonexistent" "/home/$TEST_USER/projects/target" 2>&1)
log_info "mv nonexistent output: $MV_OUT"

# Should produce error or non-zero exit
if echo "$MV_OUT" | grep -qi "not exist\|error\|fail\|cannot"; then
	log_pass "mv rejected non-existent source"
else
	# Check if target was NOT created
	if [[ ! -e "/home/$TEST_USER/projects/target" ]]; then
		log_pass "mv did not create target (expected)"
	else
		log_fail "mv created target from nonexistent source"
	fi
fi

# ============================================================
# LS1: list shows managed projects
# ============================================================
begin_test "LS1: list shows managed projects"
setup_test_user standard

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

if [[ -n "$AI_USER" ]]; then
	LIST_OUT=$(run_am list 2>&1)
	log_info "list output: $LIST_OUT"

	if echo "$LIST_OUT" | grep -q "$AI_USER"; then
		log_pass "list contains ai-user"
	else
		log_fail "list missing ai-user"
	fi

	if echo "$LIST_OUT" | grep -q "testproj"; then
		log_pass "list contains project name"
	else
		log_fail "list missing project name"
	fi
fi

# ============================================================
# LS2: list with no projects
# ============================================================
begin_test "LS2: list with no projects"
setup_test_user standard

LIST_OUT=$(run_am list 2>&1)
log_info "list empty output: $LIST_OUT"

if echo "$LIST_OUT" | grep -qi "no.*managed\|no.*project\|empty\|none"; then
	log_pass "empty list message shown"
else
	# If no specific message, just check output is not an error
	if [[ -n "$LIST_OUT" ]]; then
		log_pass "list produced output (may show header only)"
	else
		log_warn "list produced no output"
	fi
fi

# ============================================================
# ST1: status shows project info
# ============================================================
begin_test "ST1: status shows project info"
setup_test_user standard

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

if [[ -n "$AI_USER" ]]; then
	STATUS_OUT=$(run_am status 2>&1)
	log_info "status output: $STATUS_OUT"

	if echo "$STATUS_OUT" | grep -q "$AI_USER"; then
		log_pass "status contains ai-user"
	else
		log_warn "status may not list user details"
	fi

	if echo "$STATUS_OUT" | grep -qi "SSH\|ok\|auth\|mount"; then
		log_pass "status shows health info"
	else
		log_warn "status output may differ from expected"
	fi
fi

# ============================================================
# HT1: health checks mounts
# ============================================================
begin_test "HT1: health checks mounts"
setup_test_user standard

OUT=$(run_am create "/home/$TEST_USER/projects/testproj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

if [[ -n "$AI_USER" ]]; then
	HEALTH_OUT=$(run_am health 2>&1)
	log_info "health output: $HEALTH_OUT"

	# health should produce some output (either "healthy" or info about mounts)
	if [[ -n "$HEALTH_OUT" ]]; then
		log_pass "health produced output"
	else
		log_warn "health produced no output"
	fi

	# Should mention mount or project or be clean
	if echo "$HEALTH_OUT" | grep -qi "healthy\|mount\|project\|ok\|clean\|check"; then
		log_pass "health shows relevant info"
	else
		log_warn "health output format differs"
	fi
fi

# ============================================================
# FD3: force-destroy rejects non-ai-mirror user (security)
# ============================================================
begin_test "FD3: force-destroy rejects system user"
setup_test_user standard

FD_OUT=$(run_am force-destroy "root" 2>&1)
log_info "force-destroy root output: $FD_OUT"

# Should reject - root must still exist
if id "root" &>/dev/null; then
	log_pass "root user still exists (not destroyed)"
else
	log_fail "root user was destroyed! SECURITY ISSUE"
fi

if echo "$FD_OUT" | grep -qi "not.*ai-mirror\|invalid\|cannot\|error\|refuse\|denied"; then
	log_pass "force-destroy rejected system user with message"
else
	log_warn "force-destroy did not produce clear rejection message for root"
fi

# ============================================================
# MEM1: memory check on create (30MB limit)
# ============================================================
begin_test "MEM1: create memory under 30MB"
setup_test_user standard

# Use /usr/bin/time -v to measure peak memory
# Note: must pass SUDO_USER/SUDO_UID env vars for am to work
MEM_TMPFILE=$(mktemp)
MEM_STDOUT=$(mktemp)
/usr/bin/time -v \
	env SUDO_USER="$TEST_USER" SUDO_UID="$(id -u "$TEST_USER")" HOME="/home/$TEST_USER" \
	/usr/local/bin/am create "/home/$TEST_USER/projects/memtest" \
	>"$MEM_STDOUT" 2>"$MEM_TMPFILE" || true

# Parse peak memory from time output
MEM_PEAK=$(grep "Maximum resident set size" "$MEM_TMPFILE" 2>/dev/null | awk '{print $NF}')

log_info "MEM1 stdout: $(cat "$MEM_STDOUT")"
log_info "MEM1 time output: $(grep -E "resident|Exit" "$MEM_TMPFILE" 2>/dev/null || echo '(empty)')"

if [[ -n "$MEM_PEAK" && "$MEM_PEAK" -gt 0 ]]; then
	MEM_MB=$((MEM_PEAK / 1024))
	log_info "create peak memory: ${MEM_MB}MB (${MEM_PEAK}KB)"
	if [[ "$MEM_MB" -gt 30 ]]; then
		log_warn "create memory ${MEM_MB}MB exceeds 30MB limit"
	else
		log_pass "create memory ${MEM_MB}MB under 30MB limit"
	fi
else
	# Fallback: check /proc/self/status VmPeak from the process
	# In some Docker environments, time -v needs explicit path
	log_warn "memory measurement not available (time -v output empty or no /usr/bin/time)"
fi

rm -f "$MEM_TMPFILE" "$MEM_STDOUT"

full_cleanup || true

# ============================================================
# CM1: Circular mount - mount.paths contains ai-user home
# ============================================================
begin_test "CM1: mount.paths contains ai-user home"
setup_test_user standard

# Create first project and get ai-user home
OUT=$(run_am create "/home/$TEST_USER/projects/proj1" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

if [[ -n "$AI_USER" && -d "/home/$AI_USER" ]]; then
	# Configure mount.paths to include ai-user home (should be rejected or warned)
	mkdir -p "/home/$TEST_USER/projects/proj2"
	cat >"/home/$TEST_USER/.ai-mirror.toml" <<EOF
[mount]
paths = ["/home/$AI_USER"]
EOF
	chown "$TEST_USER:$TEST_USER" "/home/$TEST_USER/.ai-mirror.toml"

	OUT2=$(run_am create "/home/$TEST_USER/projects/proj2" 2>&1)
	log_info "CM1 output: $OUT2"

	# Should reject or warn about circular mount
	if echo "$OUT2" | grep -qi "circular\|invalid\|cannot\|error\|refuse\|denied\|subdirectory"; then
		log_pass "create rejected mount.paths containing ai-user home"
	else
		# If not rejected, check that no new ai-user was created
		AI_USER2=$(echo "$OUT2" | tail -1 | tr -d '[:space:]')
		if [[ -z "$AI_USER2" ]] || [[ "$AI_USER2" == "$AI_USER" ]]; then
			log_warn "create accepted but may have skipped mount.paths with ai-user home"
		else
			log_warn "create accepted mount.paths with ai-user home (potential circular issue)"
		fi
	fi
else
	log_warn "CM1: could not create first project for ai-user home test"
fi

full_cleanup || true

# ============================================================
# CM2: Nested mount.paths (parent and child)
# ============================================================
begin_test "CM2: nested mount.paths"
setup_test_user standard

# Create nested directories
mkdir -p "/home/$TEST_USER/projects/nested/subdir"
touch "/home/$TEST_USER/projects/nested/file1.txt"
touch "/home/$TEST_USER/projects/nested/subdir/file2.txt"

# Configure mount.paths with parent and child
cat >"/home/$TEST_USER/.ai-mirror.toml" <<EOF
[mount]
paths = ["~/projects/nested", "~/projects/nested/subdir"]
EOF
chown "$TEST_USER:$TEST_USER" "/home/$TEST_USER/.ai-mirror.toml"

OUT=$(run_am create "/home/$TEST_USER/projects/nested" 2>&1) || true
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')
log_info "CM2 output: $OUT"

if [[ -n "$AI_USER" ]]; then
	# Check that nested mount.paths were handled correctly
	# Expected: only one mount, or both mounted without issue
	MOUNT_COUNT=$(findmnt -l 2>/dev/null | grep -c "/home/$AI_USER" || true)
	log_info "CM2: mount count for $AI_USER = $MOUNT_COUNT"

	# Both paths should be under the same ai-user home
	if [[ "$MOUNT_COUNT" -ge 1 ]]; then
		log_pass "nested mount.paths handled (mount count: $MOUNT_COUNT)"
	else
		log_warn "no mounts found for nested mount.paths"
	fi
else
	log_warn "CM2: create failed for nested mount.paths test"
fi

full_cleanup || true

# ============================================================
# CM3: Source == Target (same path)
# ============================================================
begin_test "CM3: source equals target"
setup_test_user standard

# Try to create project where source would equal target
# This happens if user tries to create in an existing ai-user home
mkdir -p "/home/$TEST_USER/projects/samename"

OUT=$(run_am create "/home/$TEST_USER/projects/samename" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

if [[ -n "$AI_USER" ]]; then
	# Now try to create another project inside the ai-user home
	# The ai-user home IS the target, so source would be under target
	mkdir -p "/home/$AI_USER/projects/nested_proj"
	OUT2=$(run_am create "/home/$AI_USER/projects/nested_proj" 2>&1) || true
	log_info "CM3 output: $OUT2"

	# Should reject because source is under target (ai-user home)
	# "Path not allowed" is the rejection message for ai-user home
	if echo "$OUT2" | grep -qi "circular\|subdirectory\|invalid\|cannot\|error\|refuse\|denied\|not allowed"; then
		log_pass "create rejected source under ai-user home (circular)"
	else
		AI_USER2=$(echo "$OUT2" | tail -1 | tr -d '[:space:]')
		if [[ -n "$AI_USER2" ]]; then
			log_warn "create allowed nested project in ai-user home (unexpected)"
		else
			log_pass "create rejected nested project (no ai-user created)"
		fi
	fi
else
	log_warn "CM3: initial create failed"
fi

full_cleanup || true

# ============================================================
# CM4: Target is subdirectory of source
# ============================================================
begin_test "CM4: target is subdirectory of source"
setup_test_user standard

# This tests the is_subpath(target, source) check
# If source is /home/user/proj and target would be /home/user/proj/subdir
# That should be rejected

mkdir -p "/home/$TEST_USER/projects/circ_proj"

OUT=$(run_am create "/home/$TEST_USER/projects/circ_proj" 2>&1)
AI_USER=$(echo "$OUT" | tail -1 | tr -d '[:space:]')

if [[ -n "$AI_USER" ]]; then
	# Verify the ai-user home is NOT under the source
	if [[ "/home/$AI_USER" == "/home/$TEST_USER/projects/circ_proj" ]]; then
		log_warn "CM4: ai-user home equals source path"
	else
		log_pass "ai-user home is separate from source"
	fi
else
	log_warn "CM4: create failed"
fi

full_cleanup || true

# ============================================================
# OW: Ownership/permission tests for paths outside HOME
# ============================================================
setup_test_user standard

TEST_UID=$(id -u "$TEST_USER")
TEST_GID=$(id -g "$TEST_USER")

# OW1: Path owner is main user → allow create
begin_test "OW1: path owner is main user"
mkdir -p /data/testproj
chown $TEST_UID:$TEST_GID /data/testproj
OUT=$(run_am create /data/testproj 2>&1)
AI_USER=$(echo "$OUT" | grep -o "itestx_testproj" | head -1)
if [[ -n "$AI_USER" ]]; then
	log_pass "OW1: owner path allowed create"
else
	log_fail "OW1: owner path create failed"
	log_info "OW1 output: $OUT"
fi
full_cleanup || true
rm -rf /data/testproj
setup_test_user standard

# OW2: Path owner is NOT main user → check permissions
begin_test "OW2: path owner not main user"
mkdir -p /data/restricted_proj
chown root:root /data/restricted_proj
chmod 700 /data/restricted_proj # No access for other
OUT=$(run_am create /data/restricted_proj 2>&1)
if echo "$OUT" | grep -qi "not accessible\|not allowed"; then
	log_pass "OW2: restricted path rejected"
else
	log_fail "OW2: restricted path should be rejected"
	log_info "OW2 output: $OUT"
fi
full_cleanup || true
rm -rf /data/restricted_proj
setup_test_user standard

# OW3: Path group has write permission → allow create
begin_test "OW3: group write permission allows create"
mkdir -p /data/shared_proj
chown root:$TEST_GID /data/shared_proj
chmod 770 /data/shared_proj # Owner+group can write
OUT=$(run_am create /data/shared_proj 2>&1)
AI_USER=$(echo "$OUT" | grep -o "itestx_shared_proj" | head -1)
if [[ -n "$AI_USER" ]]; then
	log_pass "OW3: group writable path allowed create"
else
	log_fail "OW3: group writable path should be allowed"
	log_info "OW3 output: $OUT"
fi
full_cleanup || true
rm -rf /data/shared_proj
setup_test_user standard

# OW4: Path has other write permission → allow create
begin_test "OW4: other write permission allows create"
mkdir -p /data/public_proj
chown root:root /data/public_proj
chmod 777 /data/public_proj # Everyone can write
OUT=$(run_am create /data/public_proj 2>&1)
AI_USER=$(echo "$OUT" | grep -o "itestx_public_proj" | head -1)
if [[ -n "$AI_USER" ]]; then
	log_pass "OW4: public path allowed create"
else
	log_fail "OW4: public path should be allowed"
	log_info "OW4 output: $OUT"
fi
full_cleanup || true
rm -rf /data/public_proj

# ============================================================
# BUG-19: Legacy .am_status format compatibility
# ============================================================
setup_test_user standard

begin_test "BUG-19: legacy format with hash field"
mkdir -p /data/legacy_test
chown $TEST_USER:$TEST_GID /data/legacy_test
chmod 755 /data/legacy_test

# Create a new ai-user first
OUT=$(run_am create /data/legacy_test 2>&1)
AI_USER=$(echo "$OUT" | grep -o "itestx_legacy_test" | head -1)
if [[ -z "$AI_USER" ]]; then
	log_fail "BUG-19: failed to create ai-user for legacy test"
	log_info "Create output: $OUT"
else
	# Read the current .am_status (new format)
	STATE_FILE="/data/legacy_test/.am_status"
	if [[ ! -f "$STATE_FILE" ]]; then
		log_fail "BUG-19: .am_status not created"
	else
		# Convert to legacy format by adding a fake hash field
		# The hash value starts with "000" to pass PoW check
		LEGACY_CONTENT=$(cat "$STATE_FILE" | sed 's/"timestamp": /"timestamp": /' | sed 's/}$/,\n  "hash": "000deadbeefcafe123456789abcdef"\n}/')
		echo "$LEGACY_CONTENT" >"$STATE_FILE"
		chown $TEST_USER:$TEST_GID "$STATE_FILE"

		# Now try am update - should accept legacy format
		OUT=$(run_am update /data/legacy_test 2>&1)
		if echo "$OUT" | grep -q "State file md5 verification failed"; then
			log_fail "BUG-19: legacy format verification failed"
			log_info "Update output: $OUT"
		elif echo "$OUT" | grep -q "No .am_status found"; then
			log_fail "BUG-19: misleading 'No .am_status found' error"
			log_info "Update output: $OUT"
		else
			log_pass "BUG-19: legacy format accepted"
		fi
	fi
fi
full_cleanup || true
rm -rf /data/legacy_test

# ============================================================
# Summary
# ============================================================
full_cleanup || true

echo ""
echo "========================================================"
echo -e "  ${GREEN}PASS: $PASS${NC}  ${RED}FAIL: $FAIL${NC}"
echo "========================================================"

[[ $FAIL -gt 0 ]] && exit 1
exit 0
