#!/usr/bin/env bash
#
# ai-mirror-tidy.sh — Build, test, report
# Builds a Docker image with ai-mirror, runs all create+cd tests, reports PASS/FAIL
#
# Usage: bash tests/ai-mirror-tidy.sh
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJ_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
IMAGE_NAME="ai-mirror-test"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
LOG_DIR="$PROJ_DIR/log"
LOG_FILE="$LOG_DIR/test_${TIMESTAMP}.log"

mkdir -p "$LOG_DIR"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

tee_log() { tee -a "$LOG_FILE"; }

echo "" | tee_log
echo -e "${BOLD}========================================================${NC}" | tee_log
echo -e "${BOLD}  ai-mirror Test Runner${NC}" | tee_log
echo -e "${BOLD}  $(date)${NC}" | tee_log
echo -e "${BOLD}========================================================${NC}" | tee_log
echo -e "  Log: $LOG_FILE" | tee_log
echo "" | tee_log

# ============================================================
# Phase 1: Build Docker image
# ============================================================
echo -e "${CYAN}[Phase 1] Building Docker image: $IMAGE_NAME${NC}" | tee_log

BUILD_START=$(date +%s)

if ! docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile.base" "$PROJ_DIR" 2>&1 | tee_log | tail -5; then
	echo -e "${RED}[FATAL] Docker build failed. See log: $LOG_FILE${NC}" | tee_log
	exit 1
fi

BUILD_END=$(date +%s)
echo -e "${GREEN}[OK] Build completed in $((BUILD_END - BUILD_START))s${NC}" | tee_log
echo "" | tee_log

# ============================================================
# Phase 2: Run all tests in container
# ============================================================
echo -e "${CYAN}[Phase 2] Running comprehensive test suite${NC}" | tee_log
echo "" | tee_log

TEST_START=$(date +%s)

# Run the test script inside the container
# --privileged needed for useradd/mount/ssh
TEST_OUTPUT=$(docker run --rm --privileged "$IMAGE_NAME" \
	bash /build/tests/test_create_cd_all.sh 2>&1)

TEST_EXIT=$?

TEST_END=$(date +%s)
echo "$TEST_OUTPUT" | tee_log

echo "" | tee_log
echo -e "  Test duration: $((TEST_END - TEST_START))s" | tee_log

# ============================================================
# Phase 3: Parse and display results
# ============================================================
echo "" | tee_log
echo -e "${BOLD}========================================================${NC}" | tee_log
echo -e "${BOLD}  TEST RESULTS SUMMARY${NC}" | tee_log
echo -e "${BOLD}========================================================${NC}" | tee_log
echo "" | tee_log

# Extract PASS/FAIL counts from test output
TOTAL_PASS=$(echo "$TEST_OUTPUT" | grep -oP '\[PASS\]' | wc -l)
TOTAL_FAIL=$(echo "$TEST_OUTPUT" | grep -oP '\[FAIL\]' | wc -l)
TOTAL_WARN=$(echo "$TEST_OUTPUT" | grep -oP '\[WARN\]' | wc -l)

# Per-scenario results
echo -e "${BOLD}  Per-scenario breakdown:${NC}" | tee_log
echo "" | tee_log

SCENARIOS=$(echo "$TEST_OUTPUT" | grep -oP '(?=== ).*(?= ===)' | sort -u)
while IFS= read -r scenario; do
	[[ -z "$scenario" ]] && continue
	# Get lines between this scenario header and the next
	BLOCK=$(echo "$TEST_OUTPUT" | sed -n "/=== $scenario ===/,/=== /{ //!p }")
	S_PASS=$(echo "$BLOCK" | grep -oP '\[PASS\]' | wc -l)
	S_FAIL=$(echo "$BLOCK" | grep -oP '\[FAIL\]' | wc -l)
	S_WARN=$(echo "$BLOCK" | grep -oP '\[WARN\]' | wc -l)
	if [[ "$S_FAIL" -gt 0 ]]; then
		echo -e "  ${RED}$scenario${NC}  PASS=$S_PASS  FAIL=$S_FAIL  WARN=$S_WARN" | tee_log
		# Show failed test names
		echo "$BLOCK" | grep '\[FAIL\]' | sed 's/^/    /' | tee_log
	elif [[ "$S_WARN" -gt 0 ]]; then
		echo -e "  ${YELLOW}$scenario${NC}  PASS=$S_PASS  FAIL=$S_FAIL  WARN=$S_WARN" | tee_log
	else
		echo -e "  ${GREEN}$scenario${NC}  PASS=$S_PASS  FAIL=$S_FAIL  WARN=$S_WARN" | tee_log
	fi
done <<<"$SCENARIOS"

echo "" | tee_log
echo -e "${BOLD}  Total: ${GREEN}PASS=$TOTAL_PASS${NC}  ${RED}FAIL=$TOTAL_FAIL${NC}  ${YELLOW}WARN=$TOTAL_WARN${NC}" | tee_log
echo -e "${BOLD}  Duration: Build=$((BUILD_END - BUILD_START))s  Test=$((TEST_END - TEST_START))s${NC}" | tee_log
echo -e "${BOLD}  Log: $LOG_FILE${NC}" | tee_log
echo -e "${BOLD}========================================================${NC}" | tee_log

if [[ $TOTAL_FAIL -gt 0 ]]; then
	echo "" | tee_log
	echo -e "${RED}${BOLD}  FAILED: $TOTAL_FAIL test(s) failed. Review log above.${NC}" | tee_log
	echo "" | tee_log

	# List all failed tests
	echo -e "${RED}  Failed tests:${NC}" | tee_log
	echo "$TEST_OUTPUT" | grep '\[FAIL\]' | sed 's/^/    /' | tee_log
	echo "" | tee_log

	# ============================================================
	# Phase 4: Generate issue files for failed tests
	# ============================================================
	echo -e "${CYAN}[Phase 4] Generating issue files for failed tests${NC}" | tee_log
	echo "" | tee_log

	ISSUE_DIR="$PROJ_DIR/issues"
	mkdir -p "$ISSUE_DIR"
	TODAY=$(date +%Y-%m-%d)

	# Parse each scenario and its failures
	CURRENT_SCENARIO=""
	declare -A SCENARIO_FAILS

	while IFS= read -r line; do
		# Detect scenario header: "=== S1: User has NO SSH key ==="
		if [[ "$line" =~ ^===\ (.+)\ ===$ ]]; then
			CURRENT_SCENARIO="${BASH_REMATCH[1]}"
		fi
		# Detect FAIL line: "  [FAIL] some test name"
		if [[ "$line" =~ \[FAIL\]\ (.+) ]]; then
			FAIL_TEST="${BASH_REMATCH[1]}"
			if [[ -n "$CURRENT_SCENARIO" ]]; then
				SCENARIO_FAILS["$CURRENT_SCENARIO"]+="|$FAIL_TEST"
			fi
		fi
	done <<<"$TEST_OUTPUT"

	# Generate issue files
	ISSUE_COUNT=0
	for scenario in "${!SCENARIO_FAILS[@]}"; do
		# Sanitize scenario name for filename (replace spaces and special chars with hyphens)
		SCENARIO_SAFE=$(echo "$scenario" | sed 's/[: ]/-/g' | sed 's/[^a-zA-Z0-9_-]/-/g' | tr '[:upper:]' '[:lower:]')
		ISSUE_FILE="$ISSUE_DIR/${TODAY}-${SCENARIO_SAFE}.md"

		# Extract output block for this scenario
		SCENARIO_BLOCK=$(echo "$TEST_OUTPUT" | sed -n "/=== $scenario ===/,/=== /{ //!p }")

		# Build issue content
		cat >"$ISSUE_FILE" <<ISSUE_EOF
# Test Failure: $scenario

## 严重性
High

## 发现时间
$(date)

## 失败项
$(echo "${SCENARIO_FAILS[$scenario]}" | tr '|' '\n' | sed 's/^/- /')

## 测试输出片段

\`\`\`
$SCENARIO_BLOCK
\`\`\`

## 建议
检查相关代码逻辑，修复上述失败项。
ISSUE_EOF

		ISSUE_COUNT=$((ISSUE_COUNT + 1))
		echo -e "  ${YELLOW}Generated: ${ISSUE_FILE}${NC}" | tee_log
	done

	echo "" | tee_log
	echo -e "${BOLD}  Generated $ISSUE_COUNT issue file(s) in $ISSUE_DIR${NC}" | tee_log
	echo "" | tee_log

	exit 1
fi

echo "" | tee_log
echo -e "${GREEN}${BOLD}  ALL TESTS PASSED${NC}" | tee_log
echo "" | tee_log
exit 0
