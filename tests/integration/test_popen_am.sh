#!/usr/bin/env bash
# Test popen execution of am mv command
# This simulates the scenario where a program uses popen() to execute:
#   source /etc/profile.d/am.sh && am mv "test.md" "/path/"

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log() { echo -e "${GREEN}[test]${NC} $*"; }
info() { echo -e "${CYAN}[info]${NC} $*"; }
warn() { echo -e "${YELLOW}[warn]${NC} $*"; }
error() { echo -e "${RED}[error]${NC} $*" >&2; }

# Test helper: simulate popen() behavior
# popen() uses /bin/sh by default, not bash!
test_popen_sh() {
	local cmd="$1"
	info "Testing popen with /bin/sh: $cmd"
	# popen() internally does: /bin/sh -c "cmd"
	local result
	result=$(sh -c "$cmd" 2>&1) || true
	echo "$result"
}

test_popen_bash() {
	local cmd="$1"
	info "Testing popen with bash: $cmd"
	# Alternative: use bash explicitly
	local result
	result=$(bash -c "$cmd" 2>&1) || true
	echo "$result"
}

# Main test
main() {
	log "=== popen am mv Test ==="

	# Setup test environment (simulate ai-mirror installed)
	info "Setting up test files..."

	# Create test file
	echo "test content" >/tmp/popen_test.md

	# Create destination directory (simulating ai-user directory)
	mkdir -p /tmp/popen_dst

	log "=== Test 1: popen() default (sh -c) ==="
	# This will FAIL because sh doesn't support bash functions
	local cmd1='source /etc/profile.d/am.sh && am mv "/tmp/popen_test.md" "/tmp/popen_dst/"'
	local result1
	result1=$(test_popen_sh "$cmd1")
	if echo "$result1" | grep -q "am: not found\|source: not found\|syntax error"; then
		warn "Expected failure: sh doesn't support bash functions"
		info "Output: $result1"
	else
		info "Unexpected result: $result1"
	fi

	log "=== Test 2: popen() with bash -c ==="
	# This should work with bash
	local cmd2='source /etc/profile.d/am.sh && am mv "/tmp/popen_test.md" "/tmp/popen_dst/"'
	local result2
	result2=$(test_popen_bash "$cmd2")
	info "Output: $result2"

	log "=== Test 3: Direct binary call (recommended solution) ==="
	# This is the CORRECT way to call from popen()
	# Use the binary directly, not the shell function
	local cmd3='/usr/local/bin/ai-mirror-bin mv "/tmp/popen_test.md" "/tmp/popen_dst/"'
	info "Direct binary: $cmd3"

	log "=== Analysis ==="
	info ""
	info "ROOT CAUSE:"
	info "  1. popen() uses /bin/sh -c by default"
	info "  2. sh doesn't support bash functions (am is a bash function)"
	info "  3. Even with bash, source in subshell doesn't affect caller"
	info ""
	info "SOLUTION:"
	info "  1. Call /usr/local/bin/ai-mirror-bin directly (not am function)"
	info "  2. Or use: bash -c 'source /etc/profile.d/am.sh && am ...'"
	info "  3. Handle sudo yourself (am.sh adds sudo for non-root)"
	info ""

	# Cleanup
	rm -f /tmp/popen_test.md /tmp/popen_dst/popen_test.md 2>/dev/null || true
	rm -rf /tmp/popen_dst 2>/dev/null || true
}

main "$@"
