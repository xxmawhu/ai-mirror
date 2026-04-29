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
}
log_info() { echo -e "  ${CYAN}[INFO]${NC} $1"; }
log_warn() { echo -e "  ${YELLOW}[WARN]${NC} $1"; }

begin_test() {
	CURRENT_TEST="$1"
	echo ""
	echo -e "${YELLOW}=== $1 ===${NC}"
}

# Run am as root with SUDO_USER=testuser
run_am() { SUDO_USER="$TEST_USER" HOME="/home/$TEST_USER" /usr/local/bin/am "$@" 2>&1; }

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
	# Remove all test ai-users
	if id "$TEST_USER" &>/dev/null; then
		local prefix="i${TEST_USER}_"
		while IFS= read -r line; do
			local uname
			uname=$(echo "$line" | cut -d: -f1)
			if [[ "$uname" == ${prefix}* ]]; then
				userdel -r "$uname" 2>/dev/null || true
				groupdel "$uname" 2>/dev/null || true
			fi
		done </etc/passwd
		userdel -r "$TEST_USER" 2>/dev/null || true
	fi
	rm -rf /home/$TEST_USER /tmp/test-project-* 2>/dev/null || true
}

# Create test user with optional SSH setup
# Usage: setup_test_user [none|standard|custom:<path>]
setup_test_user() {
	local key_setup="${1:-none}"
	full_cleanup

	useradd -m -s /bin/bash "$TEST_USER" 2>/dev/null || true
	mkdir -p "/home/$TEST_USER/projects/testproj"
	chown -R "$TEST_USER:$TEST_USER" "/home/$TEST_USER"

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
		chown -R "$TEST_USER:$TEST_USER" "/home/$TEST_USER"
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
			# Check if id_ed25519 key is in authorized_keys
			if grep -q "$PUB2" "$AI_HOME/.ssh/authorized_keys" 2>/dev/null; then
				log_warn "user's personal key (id_ed25519) is in ai-user's authorized_keys"
				log_warn "SECURITY: ai-user can SSH to any server that trusts id_ed25519!"
				log_fail "SECURITY: ai_default_key grants ai-user access to main user's servers"
			else
				log_pass "user's personal key NOT in authorized_keys (safe)"
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

OUT=$(run_am create "/tmp/evil-project" 2>&1)
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
# Summary
# ============================================================
full_cleanup

echo ""
echo "========================================================"
echo -e "  ${GREEN}PASS: $PASS${NC}  ${RED}FAIL: $FAIL${NC}"
echo "========================================================"

[[ $FAIL -gt 0 ]] && exit 1
exit 0
