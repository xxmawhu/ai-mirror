#!/usr/bin/env bash
#
# Unit tests for scripts/commit-hook.sh
# Mocked: clang-format, cmake, run_tests.sh
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
	mkdir -p "$FAKE_DIR/src"
	mkdir -p "$FAKE_DIR/include"
	mkdir -p "$FAKE_DIR/tests"
	mkdir -p "$FAKE_DIR/.git/hooks"

	# Copy the real commit-hook.sh
	cp "$PROJECT_DIR/scripts/commit-hook.sh" "$FAKE_DIR/scripts/commit-hook.sh"
	chmod +x "$FAKE_DIR/scripts/commit-hook.sh"
}

cleanup_fake_project() {
	rm -rf "$FAKE_DIR"
}

# ============================================================
# Test 1: clang-format not installed → skip gracefully
# ============================================================
test_clang_format_missing() {
	echo ""
	echo "=== Test 1: clang-format missing → skip ==="

	# Cannot truly mock clang-format missing on systems where it exists
	if command -v clang-format &>/dev/null; then
		echo "  [SKIP] clang-format installed, cannot test missing scenario"
		return
	fi

	setup_fake_project

	OUTPUT=$(bash "$FAKE_DIR/scripts/commit-hook.sh" 2>&1) || true

	if echo "$OUTPUT" | grep -q "SKIPPED.*clang-format"; then
		ok "clang-format skip message shown"
	else
		fail "clang-format skip message missing"
	fi

	cleanup_fake_project
}

# ============================================================
# Test 2: clang-format detects unformatted file → FAIL
# ============================================================
test_clang_format_detects_bad_file() {
	echo ""
	echo "=== Test 2: clang-format detects bad file ==="

	if ! command -v clang-format &>/dev/null; then
		echo "  [SKIP] clang-format not installed"
		return
	fi

	setup_fake_project

	# Create a badly formatted C++ file
	cat >"$FAKE_DIR/src/test.cpp" <<'CPP'
int    main (   )  {
int   x=1+2;
return 0;}
CPP

	OUTPUT=$(bash "$FAKE_DIR/scripts/commit-hook.sh" 2>&1) || true

	if echo "$OUTPUT" | grep -q "FAILED.*clang-format"; then
		ok "clang-format detects unformatted file"
	else
		fail "clang-format did not report failure"
		echo "$OUTPUT" | tail -10
	fi

	cleanup_fake_project
}

# ============================================================
# Test 3: clang-format passes on formatted file → PASS
# ============================================================
test_clang_format_passes_good_file() {
	echo ""
	echo "=== Test 3: clang-format passes formatted file ==="

	if ! command -v clang-format &>/dev/null; then
		echo "  [SKIP] clang-format not installed"
		return
	fi

	setup_fake_project

	# Create a properly formatted C++ file
	echo 'int main() { return 0; }' >"$FAKE_DIR/src/test.cpp"

	# Need a passing run_tests.sh to get exit 0
	echo '#!/bin/bash' >"$FAKE_DIR/tests/run_tests.sh"
	echo 'exit 0' >>"$FAKE_DIR/tests/run_tests.sh"
	chmod +x "$FAKE_DIR/tests/run_tests.sh"

	OUTPUT=$(bash "$FAKE_DIR/scripts/commit-hook.sh" 2>&1)
	EXIT_CODE=$?

	if echo "$OUTPUT" | grep -q "PASSED.*clang-format"; then
		ok "clang-format passes formatted file"
	else
		fail "clang-format did not pass"
		echo "$OUTPUT" | tail -10
	fi

	cleanup_fake_project
}

# ============================================================
# Test 4: run_tests.sh missing → skip gracefully
# ============================================================
test_run_tests_missing() {
	echo ""
	echo "=== Test 4: run_tests.sh missing → skip ==="

	setup_fake_project

	# No tests/run_tests.sh created
	OUTPUT=$(bash "$FAKE_DIR/scripts/commit-hook.sh" 2>&1) || true

	if echo "$OUTPUT" | grep -q "SKIPPED.*run_tests.sh"; then
		ok "run_tests.sh skip message shown"
	else
		fail "run_tests.sh skip message missing"
		echo "$OUTPUT" | tail -10
	fi

	cleanup_fake_project
}

# ============================================================
# Test 5: run_tests.sh fails → commit blocked (exit 1)
# ============================================================
test_run_tests_fails() {
	echo ""
	echo "=== Test 5: run_tests.sh fails → commit blocked ==="

	setup_fake_project

	# Create a failing run_tests.sh
	cat >"$FAKE_DIR/tests/run_tests.sh" <<'SH'
#!/bin/bash
echo "FAIL: test case 1"
exit 1
SH
	chmod +x "$FAKE_DIR/tests/run_tests.sh"

	OUTPUT=$(bash "$FAKE_DIR/scripts/commit-hook.sh" 2>&1) || true

	if echo "$OUTPUT" | grep -q "FAILED.*unit tests"; then
		ok "unit test failure detected"
	else
		fail "unit test failure not detected"
		echo "$OUTPUT" | tail -10
	fi

	cleanup_fake_project
}

# ============================================================
# Test 6: all pass → commit allowed (exit 0)
# ============================================================
test_all_pass() {
	echo ""
	echo "=== Test 6: all pass → commit allowed ==="

	if ! command -v clang-format &>/dev/null; then
		echo "  [SKIP] clang-format not installed"
		return
	fi

	setup_fake_project

	# Properly formatted file
	echo 'int main() { return 0; }' >"$FAKE_DIR/src/test.cpp"

	# Passing run_tests.sh
	echo '#!/bin/bash' >"$FAKE_DIR/tests/run_tests.sh"
	echo 'echo "All tests passed"' >>"$FAKE_DIR/tests/run_tests.sh"
	echo 'exit 0' >>"$FAKE_DIR/tests/run_tests.sh"
	chmod +x "$FAKE_DIR/tests/run_tests.sh"

	OUTPUT=$(bash "$FAKE_DIR/scripts/commit-hook.sh" 2>&1)
	EXIT_CODE=$?

	check "$EXIT_CODE" "0" "exit code 0 on all pass"

	if echo "$OUTPUT" | grep -q "commit allowed"; then
		ok "commit allowed message shown"
	else
		fail "commit allowed message missing"
		echo "$OUTPUT" | tail -10
	fi

	cleanup_fake_project
}

# ============================================================
# Test 7: no source files → clang-format passes (nothing to check)
# ============================================================
test_no_source_files() {
	echo ""
	echo "=== Test 7: no source files → clang-format passes ==="

	if ! command -v clang-format &>/dev/null; then
		echo "  [SKIP] clang-format not installed"
		return
	fi

	setup_fake_project

	# Empty src/ and include/
	# Need a passing run_tests.sh
	echo '#!/bin/bash' >"$FAKE_DIR/tests/run_tests.sh"
	echo 'exit 0' >>"$FAKE_DIR/tests/run_tests.sh"
	chmod +x "$FAKE_DIR/tests/run_tests.sh"

	OUTPUT=$(bash "$FAKE_DIR/scripts/commit-hook.sh" 2>&1)
	EXIT_CODE=$?

	check "$EXIT_CODE" "0" "exit code 0 with empty src"

	if echo "$OUTPUT" | grep -q "PASSED.*clang-format"; then
		ok "clang-format passes with no files"
	else
		fail "clang-format did not pass with no files"
	fi

	cleanup_fake_project
}

# ============================================================
# Test 8: set -euo pipefail present in script
# ============================================================
test_set_flags() {
	echo ""
	echo "=== Test 8: set -euo pipefail present ==="

	if grep -q 'set -euo pipefail' "$PROJECT_DIR/scripts/commit-hook.sh"; then
		ok "set -euo pipefail found"
	else
		fail "set -euo pipefail missing"
	fi
}

# ============================================================
# Test 9: script uses two phases (no build phase)
# ============================================================
test_no_build_phase() {
	echo ""
	echo "=== Test 9: no build phase in commit-hook ==="

	if grep -q "Build" "$PROJECT_DIR/scripts/commit-hook.sh"; then
		fail "commit-hook.sh still contains build phase"
	else
		ok "no build phase in commit-hook.sh"
	fi

	if grep -q "Phase 1.*Code" "$PROJECT_DIR/scripts/commit-hook.sh" &&
		grep -q "Phase 2.*Unit" "$PROJECT_DIR/scripts/commit-hook.sh"; then
		ok "two-phase structure correct"
	else
		fail "two-phase structure incorrect"
	fi
}

# ============================================================
# Main
# ============================================================
main() {
	echo "=== commit-hook.sh Unit Tests ==="

	test_clang_format_missing
	test_clang_format_detects_bad_file
	test_clang_format_passes_good_file
	test_run_tests_missing
	test_run_tests_fails
	test_all_pass
	test_no_source_files
	test_set_flags
	test_no_build_phase

	echo ""
	echo "=== Results: $PASS passed, $FAIL failed ==="
	[ $FAIL -gt 0 ] && return 1
	return 0
}

main
