#!/usr/bin/env bash
#
# Unit tests for scripts/setup-hooks.sh
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

PASS=0
FAIL=0

# Test helpers
ok() {
	echo "  [PASS] $1"
	PASS=$((PASS + 1))
}
fail() {
	echo "  [FAIL] $1"
	FAIL=$((FAIL + 1))
}
check() { if [ "$1" = "$2" ]; then ok "$3"; else fail "$3 (expected='$2', got='$1')"; fi; }

# Create a fake project directory for isolated testing
setup_fake_project() {
	FAKE_DIR=$(mktemp -d)
	mkdir -p "$FAKE_DIR/scripts"
	mkdir -p "$FAKE_DIR/.git/hooks"

	# Copy the real setup-hooks.sh
	cp "$PROJECT_DIR/scripts/setup-hooks.sh" "$FAKE_DIR/scripts/setup-hooks.sh"
	chmod +x "$FAKE_DIR/scripts/setup-hooks.sh"

	# Create minimal .pre-commit-config.yaml
	cat >"$FAKE_DIR/.pre-commit-config.yaml" <<'YAML'
repos:
  - repo: https://github.com/pre-commit/pre-commit-hooks
    rev: v5.0.0
    hooks:
      - id: trailing-whitespace
YAML

	# Create minimal commit-hook.sh
	cp "$PROJECT_DIR/scripts/commit-hook.sh" "$FAKE_DIR/scripts/commit-hook.sh"
}

cleanup_fake_project() {
	rm -rf "$FAKE_DIR"
}

# ============================================================
# Test 1: --check mode detects missing hooks
# ============================================================
test_check_mode_missing_hooks() {
	echo ""
	echo "=== Test 1: --check detects missing hooks ==="

	setup_fake_project

	# No pre-commit hook installed
	OUTPUT=$(bash "$FAKE_DIR/scripts/setup-hooks.sh" --check 2>&1) || true

	if echo "$OUTPUT" | grep -q "pre-commit.*not found"; then
		ok "--check detects missing pre-commit hook"
	else
		fail "--check did not detect missing pre-commit hook"
	fi

	if echo "$OUTPUT" | grep -q "post-merge.*not found"; then
		ok "--check detects missing post-merge hook"
	else
		fail "--check did not detect missing post-merge hook"
	fi

	cleanup_fake_project
}

# ============================================================
# Test 2: --check mode passes when hooks exist
# ============================================================
test_check_mode_hooks_exist() {
	echo ""
	echo "=== Test 2: --check passes when hooks exist ==="

	setup_fake_project

	# Create fake hooks
	touch "$FAKE_DIR/.git/hooks/pre-commit"
	touch "$FAKE_DIR/.git/hooks/post-merge"

	OUTPUT=$(bash "$FAKE_DIR/scripts/setup-hooks.sh" --check 2>&1)
	EXIT_CODE=$?

	if echo "$OUTPUT" | grep -q "pre-commit.*exists"; then
		ok "--check confirms pre-commit exists"
	else
		fail "--check did not confirm pre-commit exists"
	fi

	if echo "$OUTPUT" | grep -q "post-merge.*exists"; then
		ok "--check confirms post-merge exists"
	else
		fail "--check did not confirm post-merge exists"
	fi

	cleanup_fake_project
}

# ============================================================
# Test 3: post-merge hook has correct double dirname
# ============================================================
test_post_merge_double_dirname() {
	echo ""
	echo "=== Test 3: post-merge has double dirname ==="

	setup_fake_project

	# Run setup-hooks.sh (creates post-merge)
	if command -v pre-commit &>/dev/null; then
		bash "$FAKE_DIR/scripts/setup-hooks.sh" 2>&1 || true
	else
		# Create post-merge directly if pre-commit not available
		bash "$FAKE_DIR/scripts/setup-hooks.sh" 2>&1 || true
	fi

	if [ -f "$FAKE_DIR/.git/hooks/post-merge" ]; then
		# Check for double dirname pattern
		if grep -q 'dirname.*dirname' "$FAKE_DIR/.git/hooks/post-merge"; then
			ok "post-merge uses double dirname"
		else
			fail "post-merge missing double dirname"
			cat "$FAKE_DIR/.git/hooks/post-merge"
		fi

		# Check content structure
		if grep -q 'PROJECT_DIR=' "$FAKE_DIR/.git/hooks/post-merge" &&
			grep -q 'install.sh' "$FAKE_DIR/.git/hooks/post-merge"; then
			ok "post-merge calls install.sh"
		else
			fail "post-merge missing install.sh call"
		fi
	else
		fail "post-merge hook not created"
	fi

	cleanup_fake_project
}

# ============================================================
# Test 4: post-merge hook has self-update mechanism
# ============================================================
test_post_merge_self_update() {
	echo ""
	echo "=== Test 4: post-merge has self-update ==="

	setup_fake_project

	bash "$FAKE_DIR/scripts/setup-hooks.sh" 2>&1 || true

	if [ -f "$FAKE_DIR/.git/hooks/post-merge" ]; then
		if grep -q 'setup-hooks.sh' "$FAKE_DIR/.git/hooks/post-merge"; then
			ok "post-merge has self-update (calls setup-hooks.sh)"
		else
			fail "post-merge missing self-update"
			cat "$FAKE_DIR/.git/hooks/post-merge"
		fi
	else
		fail "post-merge hook not created"
	fi

	cleanup_fake_project
}

# ============================================================
# Test 5: setup-hooks.sh updates existing post-merge (not just create)
# ============================================================
test_setup_hooks_updates_existing() {
	echo ""
	echo "=== Test 5: setup-hooks updates existing hook ==="

	setup_fake_project

	# Create an OLD post-merge hook (wrong pattern)
	cat >"$FAKE_DIR/.git/hooks/post-merge" <<'OLDHOOK'
#!/bin/bash
# OLD hook with single dirname (wrong)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"  # BUG: single dirname
echo "OLD hook"
OLDHOOK
	chmod +x "$FAKE_DIR/.git/hooks/post-merge"

	# Run setup-hooks.sh to update it
	bash "$FAKE_DIR/scripts/setup-hooks.sh" 2>&1 || true

	# Check if it was updated
	if grep -q 'dirname.*dirname' "$FAKE_DIR/.git/hooks/post-merge"; then
		ok "existing post-merge was updated to double dirname"
	else
		fail "existing post-merge was NOT updated"
		cat "$FAKE_DIR/.git/hooks/post-merge"
	fi

	cleanup_fake_project
}

# ============================================================
# Test 6: set -euo pipefail present in script
# ============================================================
test_set_flags() {
	echo ""
	echo "=== Test 6: set -euo pipefail present ==="

	if grep -q 'set -euo pipefail' "$PROJECT_DIR/scripts/setup-hooks.sh"; then
		ok "set -euo pipefail found in setup-hooks.sh"
	else
		fail "set -euo pipefail missing"
	fi
}

# ============================================================
# Test 7: commit-hook.sh existence check
# ============================================================
test_commit_hook_exists_check() {
	echo ""
	echo "=== Test 7: commit-hook.sh existence check ==="

	setup_fake_project

	bash "$FAKE_DIR/scripts/setup-hooks.sh" 2>&1 || true

	# Output should mention commit-hook.sh exists
	OUTPUT=$(bash "$FAKE_DIR/scripts/setup-hooks.sh" 2>&1) || true

	if echo "$OUTPUT" | grep -q "commit-hook.*exists"; then
		ok "setup-hooks checks commit-hook.sh existence"
	else
		fail "setup-hooks did not check commit-hook.sh"
	fi

	cleanup_fake_project
}

# ============================================================
# Test 8: .pre-commit-config.yaml existence check
# ============================================================
test_precommit_config_check() {
	echo ""
	echo "=== Test 8: .pre-commit-config.yaml check ==="

	setup_fake_project

	OUTPUT=$(bash "$FAKE_DIR/scripts/setup-hooks.sh" 2>&1) || true

	if echo "$OUTPUT" | grep -q ".pre-commit-config.yaml.*exists"; then
		ok "setup-hooks checks .pre-commit-config.yaml"
	else
		fail "setup-hooks did not check .pre-commit-config.yaml"
	fi

	cleanup_fake_project
}

# ============================================================
# Test 9: missing .pre-commit-config.yaml → error
# ============================================================
test_missing_precommit_config() {
	echo ""
	echo "=== Test 9: missing .pre-commit-config.yaml → error ==="

	setup_fake_project

	# Remove the config
	rm -f "$FAKE_DIR/.pre-commit-config.yaml"

	OUTPUT=$(bash "$FAKE_DIR/scripts/setup-hooks.sh" 2>&1) || true

	if echo "$OUTPUT" | grep -q ".pre-commit-config.yaml.*not found"; then
		ok "setup-hooks reports missing .pre-commit-config.yaml"
	else
		fail "setup-hooks did not report missing config"
	fi

	cleanup_fake_project
}

# ============================================================
# Test 10: script uses $PROJECT_DIR correctly
# ============================================================
test_project_dir_usage() {
	echo ""
	echo "=== Test 10: PROJECT_DIR usage ==="

	if grep -q 'PROJECT_DIR=' "$PROJECT_DIR/scripts/setup-hooks.sh" &&
		grep -q '\$PROJECT_DIR' "$PROJECT_DIR/scripts/setup-hooks.sh"; then
		ok "setup-hooks.sh uses PROJECT_DIR variable"
	else
		fail "setup-hooks.sh missing PROJECT_DIR usage"
	fi
}

# ============================================================
# Main
# ============================================================
main() {
	echo "=== setup-hooks.sh Unit Tests ==="

	test_check_mode_missing_hooks
	test_check_mode_hooks_exist
	test_post_merge_double_dirname
	test_post_merge_self_update
	test_setup_hooks_updates_existing
	test_set_flags
	test_commit_hook_exists_check
	test_precommit_config_check
	test_missing_precommit_config
	test_project_dir_usage

	echo ""
	echo "=== Results: $PASS passed, $FAIL failed ==="
	[ $FAIL -gt 0 ] && return 1
	return 0
}

main
