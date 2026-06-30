#!/bin/bash
# tests/docker/test_install.sh — Install.sh deep verification tests
# Runs inside Docker container after install.sh completes
set -euo pipefail

LOG_DIR="/var/log/ai-mirror-tests"
mkdir -p "$LOG_DIR"
pass_count=0
fail_count=0

pass() {
	local name="$1"
	echo "[PASS] $name"
	echo "[PASS] $name" >>"$LOG_DIR/test_install.log"
	pass_count=$((pass_count + 1))
}

fail() {
	local name="$1"
	echo "[FAIL] $name"
	echo "[FAIL] $name" >>"$LOG_DIR/test_install.log"
	fail_count=$((fail_count + 1))
}

check() {
	local name="$1"
	local cmd="$2"
	local expect="${3:-true}"
	if eval "$cmd" 2>/dev/null; then
		# command succeeded
		if "$expect"; then
			pass "$name"
		else
			fail "$name (unexpected success)"
		fi
	else
		# command failed
		if "$expect"; then
			fail "$name"
		else
			pass "$name (correctly failed)"
		fi
	fi
}

main() {
	echo "========================================"
	echo "Install.sh Deep Verification Tests"
	echo "========================================"

	# === Phase 1: Deployed files ===
	echo ""
	echo "--- Phase 1: Deployed files ---"
	check "bash_completion deployed" "test -f /etc/bash_completion.d/am"
	check "am binary deployed" "test -x /usr/local/bin/am"
	check "ai-mirror-bin deployed" "test -x /usr/local/bin/ai-mirror-bin"
	check "data dir exists" "test -d /var/lib/ai-mirror"
	check "config dir exists" "test -d /etc/ai-mirror"

	# === Phase 2: am commands functional ===
	echo ""
	echo "--- Phase 2: CLI commands ---"
	check "am --help works" "/usr/local/bin/am --help"
	check "am version works" "/usr/local/bin/am version"
	check "ai-mirror-bin --help" "/usr/local/bin/ai-mirror-bin --help"

	# === Phase 3: Syntax validation ===
	echo ""
	echo "--- Phase 3: Syntax ---"
	check "install.sh syntax" "bash -n /app/install.sh"
	check "completion syntax" "bash -n /etc/bash_completion.d/am"

	# === Phase 4: Failure scenarios ===
	echo ""
	echo "--- Phase 4: Failure handling ---"
	# install.sh build phase fails without C++ source; that is expected
	check "install.sh build fails gracefully" "bash /app/install.sh --build 2>&1" false

	# === Summary ===
	echo ""
	echo "========================================"
	echo "Results: $pass_count passed, $fail_count failed"
	echo "========================================"
	summary="$pass_count / $((pass_count + fail_count)) tests passed"
	echo "$summary" >>"$LOG_DIR/test_install.log"
	exit "$fail_count"
}

main "$@"
