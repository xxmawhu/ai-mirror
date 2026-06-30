#!/bin/bash
# Test stale mount recovery in am update
# This test simulates a stale bind mount scenario and verifies that
# am update correctly detects and fixes it.

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

# Test setup: Create a test user environment
setup() {
	log "Setup: Creating test environment"

	# Create a real user for testing (not root)
	useradd -m -s /bin/bash testuser 2>/dev/null || true

	# Set loginuid so ai-mirror can determine the real user
	TESTUID=$(id -u testuser)
	echo "$TESTUID" >/proc/self/loginuid 2>/dev/null || true
	export HOME=/home/testuser

	# Create project directory owned by testuser
	mkdir -p /home/testuser/projects/testproj
	chown -R testuser:testuser /home/testuser/projects

	# Create ai-mirror config for testuser
	mkdir -p /home/testuser/.ssh
	cat >/home/testuser/.ai-mirror.toml <<'EOF'
[user]
prefix = "ai_"
allowed_bases = ["/home/testuser"]

[ssh]
key_path = "/home/testuser/.ssh/ai-mirror"
key_type = "ed25519"
ai_default_key = "/home/testuser/.ssh/id_ed25519"

[mount]
paths = ["~/.bashrc"]
EOF
	chown testuser:testuser /home/testuser/.ai-mirror.toml

	# Create bashrc with test content
	echo "test bashrc content for stale mount test" >/home/testuser/.bashrc
	chown testuser:testuser /home/testuser/.bashrc

	# Create SSH keys
	ssh-keygen -t ed25519 -f /home/testuser/.ssh/ai-mirror -N "" -q 2>/dev/null || true
	ssh-keygen -t ed25519 -f /home/testuser/.ssh/id_ed25519 -N "" -q -C "test" 2>/dev/null || true
	chown -R testuser:testuser /home/testuser/.ssh
	chmod 700 /home/testuser/.ssh
	chmod 600 /home/testuser/.ssh/id_ed25519 2>/dev/null || true

	# Add testuser to ai-mirror group
	groupadd -f ai-mirror || true
	usermod -aG ai-mirror testuser

	ok "Setup complete"
}

# Test 1: Create ai-user and bind mount
test_create_and_mount() {
	log "Test 1: Create ai-user and verify bind mount"

	# Set loginuid and HOME for ai-mirror to recognize the user
	TESTUID=$(id -u testuser)
	echo "$TESTUID" >/proc/self/loginuid 2>/dev/null || true
	export HOME=/home/testuser

	# Run am create with proper user context
	OUTPUT=$(HOME=/home/testuser /usr/local/bin/am create /home/testuser/projects/testproj 2>&1)
	AI_USER=$(echo "$OUTPUT" | grep -oP 'Created ai-user: \K\S+' || echo "")

	if [ -z "$AI_USER" ]; then
		fail "Could not extract ai-user from output"
		echo "Output: $OUTPUT"
		return 1
	fi

	echo "  Created: $AI_USER"

	# Verify user exists
	id "$AI_USER" >/dev/null 2>&1 && ok "AI user exists" || fail "AI user not found"

	# Find AI user home
	AI_HOME=$(getent passwd "$AI_USER" | cut -d: -f6)
	MOUNT_TARGET="$AI_HOME/.bashrc"

	echo "  AI home: $AI_HOME"
	echo "  Mount target: $MOUNT_TARGET"

	# Verify bind mount exists
	if findmnt "$MOUNT_TARGET" >/dev/null 2>&1; then
		ok "Bind mount exists"
	else
		fail "Bind mount not found"
		return 1
	fi

	# Verify mount content
	if grep -q "test bashrc content" "$MOUNT_TARGET" 2>/dev/null; then
		ok "Mount content is accessible"
	else
		fail "Mount content not accessible"
		return 1
	fi

	ok "Test 1 complete"
}

# Test 2: Simulate stale mount and verify recovery
# Strategy: umount the target, replace source file content, then let am update remount
test_stale_mount_recovery() {
	log "Test 2: Stale mount recovery via am update"

	AI_HOME=$(getent passwd "$AI_USER" | cut -d: -f6)
	MOUNT_TARGET="$AI_HOME/.bashrc"
	SOURCE_FILE="/home/testuser/.bashrc"

	# Verify mount is healthy
	findmnt "$MOUNT_TARGET" >/dev/null 2>&1 && ok "Mount exists before test" || {
		fail "Mount missing"
		return
	}
	cat "$MOUNT_TARGET" >/dev/null 2>&1 && ok "Mount is accessible before test" || {
		fail "Mount already stale"
		return
	}

	# Step 1: Manually umount to simulate a stale/broken mount scenario
	umount "$MOUNT_TARGET" 2>/dev/null || {
		fail "Cannot umount for test setup"
		return
	}
	ok "Manually unmounted for test setup"

	# Step 2: Verify mount is gone
	if ! findmnt "$MOUNT_TARGET" >/dev/null 2>&1; then
		ok "Mount removed successfully"
	else
		fail "Mount still exists after umount"
		return
	fi

	# Step 3: Update source file content (simulating source change)
	echo "updated bashrc content after remount" >"$SOURCE_FILE"
	chown testuser:testuser "$SOURCE_FILE"
	ok "Source file updated with new content"

	# Step 4: Run am update to remount
	TESTUID=$(id -u testuser)
	echo "$TESTUID" >/proc/self/loginuid 2>/dev/null || true
	export HOME=/home/testuser
	echo "  Running am update..."
	HOME=/home/testuser /usr/local/bin/am update /home/testuser/projects/testproj 2>&1 | tail -10
	ok "am update executed"

	# Step 5: Verify mount is restored
	findmnt "$MOUNT_TARGET" >/dev/null 2>&1 && ok "Mount restored after am update" || fail "Mount NOT restored after am update"

	# Step 6: Verify mount content matches updated source
	if grep -q "updated bashrc content" "$MOUNT_TARGET" 2>/dev/null; then
		ok "Mount content matches updated source"
	else
		fail "Mount content does not match updated source"
	fi

	# Step 7: Verify mount is read-only
	if echo "test" >"$MOUNT_TARGET" 2>/dev/null; then
		fail "Mount is NOT read-only"
	else
		ok "Mount is read-only"
	fi

	ok "Test 2 complete"
}

# Test 3: Simulate stale mount (target inaccessible but mount shows in /proc/mounts)
# Strategy: bind mount from a temp file, then delete the temp file to create a truly stale mount
test_truly_stale_mount_recovery() {
	log "Test 3: Truly stale mount (deleted source) recovery"

	AI_HOME=$(getent passwd "$AI_USER" | cut -d: -f6)
	MOUNT_TARGET="$AI_HOME/.bashrc"
	SOURCE_FILE="/home/testuser/.bashrc"
	TEMP_SOURCE="/tmp/.bashrc_temp_stale_test"

	# Verify mount is healthy from previous test
	findmnt "$MOUNT_TARGET" >/dev/null 2>&1 && ok "Mount exists before stale simulation" || {
		fail "Mount missing"
		return
	}

	# Step 1: Unmount current mount
	umount "$MOUNT_TARGET" 2>/dev/null || {
		fail "Cannot umount for stale simulation"
		return
	}
	ok "Unmounted for stale simulation"

	# Step 2: Create a temp source file, mount it, then delete it to create a stale mount
	echo "temp content that will be deleted" >"$TEMP_SOURCE"
	mount --bind -o ro "$TEMP_SOURCE" "$MOUNT_TARGET" 2>/dev/null || {
		fail "Cannot create test bind mount"
		rm -f "$TEMP_SOURCE"
		return
	}
	ok "Created test bind mount from temp file"

	# Step 3: Verify mount is working
	cat "$MOUNT_TARGET" >/dev/null 2>&1 && ok "Test mount is accessible" || {
		fail "Test mount is already stale"
		return
	}

	# Step 4: Delete the source file — this creates a stale mount on most filesystems
	# On ext4/xfs, the mount still works until last fd closes due to Linux VFS caching
	# But we can check the behavior by verifying via /proc/mounts
	rm -f "$TEMP_SOURCE"
	ok "Deleted temp source file"

	# The key test: even if mount still shows accessible, our stale detection
	# should handle the case where mount exists but target becomes inaccessible
	# This is the exact scenario the code fix addresses

	# Step 5: Run am update — should handle stale mount scenario
	TESTUID=$(id -u testuser)
	echo "$TESTUID" >/proc/self/loginuid 2>/dev/null || true
	export HOME=/home/testuser
	echo "  Running am update to fix stale mount..."
	HOME=/home/testuser /usr/local/bin/am update /home/testuser/projects/testproj 2>&1 | tail -10
	ok "am update executed for stale recovery"

	# Step 6: Verify mount points to the correct source file
	findmnt "$MOUNT_TARGET" >/dev/null 2>&1 && ok "Mount exists after am update" || fail "Mount missing after am update"

	if cat "$MOUNT_TARGET" >/dev/null 2>&1; then
		ok "Mount is accessible after am update"
		# Verify content is from the real source file
		if grep -q "updated bashrc content" "$MOUNT_TARGET" 2>/dev/null; then
			ok "Mount content is from real source file (stale mount was replaced)"
		else
			fail "Mount content is wrong (may still be stale)"
		fi
	else
		fail "Mount is still inaccessible after am update"
	fi

	# Cleanup
	rm -f "$TEMP_SOURCE"

	ok "Test 3 complete"
}

# Cleanup
cleanup() {
	log "Cleanup"

	# Destroy ai-user if exists
	if [ -n "${AI_USER:-}" ] && id "$AI_USER" >/dev/null 2>&1; then
		/usr/local/bin/am force-destroy "$(getent passwd "$AI_USER" | cut -d: -f6)" 2>&1 || true
	fi

	# Remove test user
	userdel -r testuser 2>/dev/null || true
	groupdel testuser 2>/dev/null || true
	groupdel ai-mirror 2>/dev/null || true

	# Cleanup files
	rm -rf /home/testuser/.ai-mirror.toml /home/testuser/.ssh/ai-mirror* /home/testuser/.ssh/id_ed25519* 2>/dev/null || true

	ok "Cleanup complete"
}

# Main
main() {
	log "ai-mirror Stale Mount Recovery Test"

	setup || {
		fail "Setup failed"
		cleanup
		return 1
	}
	test_create_and_mount || {
		fail "Test 1 failed"
		cleanup
		return 1
	}
	test_stale_mount_recovery || {
		fail "Test 2 failed"
		cleanup
		return 1
	}
	test_truly_stale_mount_recovery || {
		fail "Test 3 failed"
		cleanup
		return 1
	}
	cleanup

	log "Results: $PASS passed, $FAIL failed"
	[ $FAIL -gt 0 ] && return 1
	return 0
}

main
