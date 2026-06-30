#!/bin/bash
# Test .git deep chown in am create
# Verifies that files deep inside .git/ (objects/pack/, refs/heads/feature/)
# get correct ownership after am create.

set -euo pipefail

PASS=0
FAIL=0

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

setup() {
	log "Setup"

	useradd -m -s /bin/bash gituser 2>/dev/null || true

	TESTUID=$(id -u gituser)
	echo "$TESTUID" >/proc/self/loginuid 2>/dev/null || true
	export HOME=/home/gituser

	# Create project with a fake .git directory (deep structure)
	PROJ="/home/gituser/projects/gitproj"
	mkdir -p "$PROJ"
	mkdir -p "$PROJ/.git/objects/pack"
	mkdir -p "$PROJ/.git/objects/ab"
	mkdir -p "$PROJ/.git/refs/heads/feature/sub"
	mkdir -p "$PROJ/.git/hooks"

	# Create deep files
	echo "pack data" >"$PROJ/.git/objects/pack/abc123def456.pack"
	echo "idx data" >"$PROJ/.git/objects/pack/abc123def456.idx"
	echo "loose obj" >"$PROJ/.git/objects/ab/cdef1234567890"
	echo "branch ref" >"$PROJ/.git/refs/heads/feature/sub/deep-branch"
	echo "head ref" >"$PROJ/.git/HEAD"
	echo "config" >"$PROJ/.git/config"
	echo "hook" >"$PROJ/.git/hooks/pre-commit"

	chown -R gituser:gituser "$PROJ"

	# ai-mirror config
	mkdir -p /home/gituser/.ssh
	cat >/home/gituser/.ai-mirror.toml <<'EOF'
[user]
prefix = "ai_"
allowed_bases = ["/home/gituser"]

[ssh]
key_path = "/home/gituser/.ssh/ai-mirror"
key_type = "ed25519"
ai_default_key = "/home/gituser/.ssh/id_ed25519"

[mount]
paths = ["~/.bashrc"]
EOF
	chown gituser:gituser /home/gituser/.ai-mirror.toml

	echo "bashrc" >/home/gituser/.bashrc
	chown gituser:gituser /home/gituser/.bashrc

	ssh-keygen -t ed25519 -f /home/gituser/.ssh/ai-mirror -N "" -q 2>/dev/null || true
	ssh-keygen -t ed25519 -f /home/gituser/.ssh/id_ed25519 -N "" -q -C "test" 2>/dev/null || true
	chown -R gituser:gituser /home/gituser/.ssh
	chmod 700 /home/gituser/.ssh
	chmod 600 /home/gituser/.ssh/id_ed25519 2>/dev/null || true

	groupadd -f ai-mirror || true
	usermod -aG ai-mirror gituser

	ok "Setup complete"
}

test_create_gitproj() {
	log "Test 1: am create with .git directory"

	TESTUID=$(id -u gituser)
	echo "$TESTUID" >/proc/self/loginuid 2>/dev/null || true
	export HOME=/home/gituser

	OUTPUT=$(HOME=/home/gituser /usr/local/bin/am create /home/gituser/projects/gitproj 2>&1)
	AI_USER=$(echo "$OUTPUT" | grep -oP 'Created ai-user: \K\S+' || echo "")

	if [ -z "$AI_USER" ]; then
		# Try alternate pattern
		AI_USER=$(echo "$OUTPUT" | tail -1 | xargs)
	fi

	if [ -z "$AI_USER" ] || ! id "$AI_USER" >/dev/null 2>&1; then
		fail "Could not create ai-user"
		echo "Output: $OUTPUT"
		return 1
	fi

	echo "  Created: $AI_USER"
	ok "AI user created"

	AI_HOME=$(getent passwd "$AI_USER" | cut -d: -f6)
	AI_UID=$(id -u "$AI_USER")
	AI_GID=$(id -g "$AI_USER")

	echo "  AI home: $AI_HOME"
	echo "  AI uid:gid: $AI_UID:$AI_GID"
}

test_git_deep_ownership() {
	log "Test 2: .git deep file ownership"

	AI_HOME=$(getent passwd "$AI_USER" | cut -d: -f6)
	AI_UID=$(id -u "$AI_USER")
	AI_GID=$(id -g "$AI_USER")

	check_owner() {
		local file="$1"
		local desc="$2"
		if [ ! -e "$file" ]; then
			fail "$desc: file not found ($file)"
			return
		fi
		local owner_uid
		owner_uid=$(stat -c '%u' "$file")
		local owner_gid
		owner_gid=$(stat -c '%g' "$file")
		if [ "$owner_uid" = "$AI_UID" ] && [ "$owner_gid" = "$AI_GID" ]; then
			ok "$desc: ownership correct (uid=$AI_UID gid=$AI_GID)"
		else
			fail "$desc: wrong ownership (got uid=$owner_uid gid=$owner_gid, expected uid=$AI_UID gid=$AI_GID)"
		fi
	}

	# Depth 1: .git itself
	check_owner "$AI_HOME/.git" ".git (depth 1)"

	# Depth 2: .git/objects, .git/refs, .git/hooks
	check_owner "$AI_HOME/.git/objects" ".git/objects (depth 2)"
	check_owner "$AI_HOME/.git/refs" ".git/refs (depth 2)"
	check_owner "$AI_HOME/.git/hooks" ".git/hooks (depth 2)"
	check_owner "$AI_HOME/.git/HEAD" ".git/HEAD (depth 2)"
	check_owner "$AI_HOME/.git/config" ".git/config (depth 2)"

	# Depth 3: .git/objects/pack, .git/refs/heads
	check_owner "$AI_HOME/.git/objects/pack" ".git/objects/pack (depth 3)"
	check_owner "$AI_HOME/.git/refs/heads" ".git/refs/heads (depth 3)"
	check_owner "$AI_HOME/.git/hooks/pre-commit" ".git/hooks/pre-commit (depth 3)"

	# Depth 4: .git/objects/pack/*.pack, .git/refs/heads/feature, .git/objects/ab/
	check_owner "$AI_HOME/.git/objects/pack/abc123def456.pack" ".git/objects/pack/abc.pack (depth 4)"
	check_owner "$AI_HOME/.git/objects/pack/abc123def456.idx" ".git/objects/pack/abc.idx (depth 4)"
	check_owner "$AI_HOME/.git/refs/heads/feature" ".git/refs/heads/feature (depth 4)"
	check_owner "$AI_HOME/.git/objects/ab" ".git/objects/ab (depth 4)"

	# Depth 5: .git/refs/heads/feature/sub, .git/objects/ab/cdef*
	check_owner "$AI_HOME/.git/refs/heads/feature/sub" ".git/refs/heads/feature/sub (depth 5)"
	check_owner "$AI_HOME/.git/refs/heads/feature/sub/deep-branch" ".git/.../deep-branch (depth 6)"
	check_owner "$AI_HOME/.git/objects/ab/cdef1234567890" ".git/objects/ab/cdef (depth 5)"

	ok "Test 2 complete"
}

cleanup() {
	log "Cleanup"

	if [ -n "${AI_USER:-}" ] && id "$AI_USER" >/dev/null 2>&1; then
		/usr/local/bin/am force-destroy "$(getent passwd "$AI_USER" | cut -d: -f6)" 2>&1 || true
	fi

	userdel -r gituser 2>/dev/null || true
	groupdel gituser 2>/dev/null || true
	groupdel ai-mirror 2>/dev/null || true
	rm -rf /home/gituser 2>/dev/null || true

	ok "Cleanup complete"
}

main() {
	log "ai-mirror .git Deep Chown Test"

	setup || {
		fail "Setup failed"
		cleanup
		return 1
	}
	test_create_gitproj || {
		fail "Test 1 failed"
		cleanup
		return 1
	}
	test_git_deep_ownership || {
		fail "Test 2 failed"
		cleanup
		return 1
	}
	cleanup

	log "Results: $PASS passed, $FAIL failed"
	[ $FAIL -gt 0 ] && return 1
	return 0
}

main
