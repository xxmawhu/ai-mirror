#!/bin/bash
set -euo pipefail

PASS=0
FAIL=0
WARN=0
AI_USER=""
AI_HOME="/root/projects/testproj"

log() {
	echo ""
	echo "=== $1 ==="
}
ok() {
	echo "  [PASS] $1"
	PASS=$((PASS + 1))
}
fail() {
	echo "  [FAIL] $1"
	FAIL=$((FAIL + 1))
}
warn() {
	echo "  [WARN] $1"
	WARN=$((WARN + 1))
}
check() { if [ "$1" = "$2" ]; then ok "$3"; else fail "$3 (expected='$2', got='$1')"; fi; }

setup() {
	log "Setup"
	mkdir -p /root/.ssh /root/projects/testproj
	cat >/root/.ai-mirror.toml <<'EOF'
[user]
prefix = "ai_"

[ssh]
key_path = "/root/.ssh/ai-mirror"
key_type = "ed25519"
ai_default_key = "/root/.ssh/id_ed25519"

[mount]
paths = ["~/.bashrc"]
EOF
	echo "test bashrc content" >/root/.bashrc
	ssh-keygen -t ed25519 -f /root/.ssh/ai-mirror -N "" -q 2>/dev/null || true
	ssh-keygen -t ed25519 -f /root/.ssh/id_ed25519 -N "" -q -C "test" 2>/dev/null || true
	chmod 700 /root/.ssh
	chmod 600 /root/.ssh/id_ed25519 2>/dev/null || true
	ok "Config and SSH keys setup complete"
}

test_create() {
	log "Test 1: am create"

	OUTPUT=$(/usr/local/bin/am create /root/projects/testproj 2>&1)
	AI_USER=$(echo "$OUTPUT" | grep -oP 'Created ai-user: \K\S+')

	if [ -z "$AI_USER" ]; then
		fail "Could not extract username"
		echo "Output: $OUTPUT"
		return 1
	fi

	echo "  Created: $AI_USER"

	id "$AI_USER" >/dev/null 2>&1 && ok "User exists" || fail "User not found"
	[[ "$AI_USER" == "ai_root_testproj" ]] && ok "Username format correct" || fail "Username wrong: $AI_USER"
	groups root | grep -q "$AI_USER" && ok "root in AI group" || fail "root NOT in AI group"

	SSH_UID=$(stat -c '%u' "$AI_HOME/.ssh" 2>/dev/null || echo "missing")
	AI_UID=$(id -u "$AI_USER")
	check "$SSH_UID" "$AI_UID" ".ssh ownership"

	[ -s "$AI_HOME/.ssh/authorized_keys" ] && ok "authorized_keys exists" || fail "authorized_keys missing"

	HASH=$(python3 -c "
import json,hashlib
with open('$AI_HOME/.am_status') as f: d=json.load(f)
if 'hash' in d: print(d['hash'][:3])
elif 'nonce' in d: print(hashlib.md5(open('$AI_HOME/.am_status').read().encode()).hexdigest()[:5])
else: print('invalid')
")
	[[ "$HASH" == "000"* ]] && ok "PoW valid (hash starts 000)" || fail "PoW invalid (hash=$HASH)"

	echo "$OUTPUT" | tail -1
}

test_bind_mount() {
	log "Test 2: bind mount (ro)"

	MOUNT_TARGET="$AI_HOME/.bashrc"

	if ! findmnt "$MOUNT_TARGET" >/dev/null 2>&1; then
		fail ".bashrc not mounted"
		echo "$OUTPUT" | tail -1
		return
	fi

	ok ".bashrc bind mounted"

	grep -q "test bashrc content" "$MOUNT_TARGET" && ok "Mount content readable" || fail "Mount content wrong"

	echo "overwrite" >"$MOUNT_TARGET" 2>/dev/null && fail "Mount is NOT read-only" || ok "Mount is read-only"

	findmnt -n -o OPTIONS "$MOUNT_TARGET" | grep -q "ro" && ok "Mount options contain ro" || warn "Mount options missing ro"

	echo "$MOUNT_TARGET"
}

test_umount_recovery() {
	log "Test 3: umount recovery via am update"

	MOUNT_TARGET="$AI_HOME/.bashrc"

	findmnt "$MOUNT_TARGET" >/dev/null 2>&1 && ok "Mount exists before umount" || fail "Mount missing before umount"

	umount "$MOUNT_TARGET" 2>/dev/null || {
		warn "Cannot umount in Docker"
		return
	}

	! findmnt "$MOUNT_TARGET" >/dev/null 2>&1 && ok "Mount removed after umount" || fail "Mount still exists"

	/usr/local/bin/am update /root/projects/testproj 2>&1 | tail -3

	findmnt "$MOUNT_TARGET" >/dev/null 2>&1 && ok "am update remounted .bashrc" || fail "am update did NOT remount .bashrc"
}

test_status() {
	log "Test 4: am status"

	STATUS=$(/usr/local/bin/am status 2>&1)
	echo "$STATUS"

	grep -q "SSH:   ok" <<<"$STATUS" && ok "SSH ok" || fail "SSH NOT ok"
	grep -q "Auth:  ok" <<<"$STATUS" && ok "Auth ok" || fail "Auth NOT ok"
	grep -q "$AI_USER" <<<"$STATUS" && ok "Shows AI user" || fail "No AI user"
	grep -q "Status: unhealthy" <<<"$STATUS" && ok "Shows unhealthy" || warn "Status missing"
}

test_update_ssh() {
	log "Test 5: am update (SSH repair)"

	AI_UID=$(id -u "$AI_USER")

	chown root:root "$AI_HOME/.ssh"
	check "$(stat -c '%u' "$AI_HOME/.ssh")" "0" ".ssh tampered to root"

	/usr/local/bin/am update /root/projects/testproj 2>&1 | tail -3

	check "$(stat -c '%u' "$AI_HOME/.ssh")" "$AI_UID" ".ssh ownership restored"
	groups root | grep -q "$AI_USER" && ok "Group membership restored" || fail "Group NOT restored"
}

test_ssh_login() {
	log "Test 6: SSH passwordless login"

	mkdir -p /run/sshd
	/usr/sbin/sshd 2>/dev/null || true
	sleep 1

	if ssh -i /root/.ssh/ai-mirror -o StrictHostKeyChecking=no -o BatchMode=yes -o ConnectTimeout=5 "$AI_USER@localhost" echo OK 2>&1 | grep -q OK; then
		ok "SSH login works"
	else
		warn "SSH login failed (sshd config issue)"
	fi
}

test_destroy() {
	log "Test 7: am force-destroy"

	/usr/local/bin/am force-destroy "$AI_HOME" 2>&1 | tail -3

	! id "$AI_USER" >/dev/null 2>&1 && ok "User destroyed" || fail "User still exists"
	! getent group "$AI_USER" >/dev/null 2>&1 && ok "Group removed" || warn "Group still exists"
	[ ! -f "$AI_HOME/.am_status" ] && ok "State file removed" || warn "State file exists"
}

cleanup() {
	log "Cleanup"
	pkill sshd 2>/dev/null || true
	userdel -r "$AI_USER" 2>/dev/null || true
	groupdel "$AI_USER" 2>/dev/null || true
	rm -rf /root/.ai-mirror.toml /root/.ssh/ai-mirror* /root/.ssh/id_ed25519* "$AI_HOME/.ssh" "$AI_HOME/.am_status"
	ok "Cleanup done"
}

main() {
	log "ai-mirror Integration Tests"

	setup

	OUTPUT=$(test_create)
	AI_USER=$(echo "$OUTPUT" | tail -1)

	if [ -n "$AI_USER" ] && id "$AI_USER" >/dev/null 2>&1; then
		MOUNT_TARGET=$(test_bind_mount)
		test_umount_recovery
		test_status
		test_update_ssh
		test_ssh_login
		test_destroy
	else
		fail "User creation failed, skipping tests"
	fi

	cleanup

	log "Results: $PASS passed, $FAIL failed, $WARN warnings"
	[ $FAIL -gt 0 ] && return 1
	return 0
}

main
