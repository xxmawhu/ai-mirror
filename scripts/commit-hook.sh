#!/usr/bin/env bash
#
# ai-mirror commit-hook: five-phase validation
# Phase 0: Version check (version.hpp.in must be updated)
# Phase 1: Code check (clang-format dry-run)
# Phase 1b: File permissions check (source files must be 600)
# Phase 2: Build verification (cmake)
# Phase 3: Unit tests (Docker, no host root required)
#
# [log-review] 日志输出到 ./log/hook/ (Rule 2/9)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
LOG_DIR="$PROJECT_DIR/log/hook"
LOG_FILE="$LOG_DIR/commit-$(date +%Y-%m-%d).log"

# Ensure log directory exists
mkdir -p "$LOG_DIR"

# Tee all output to log file (Rule 2: screen output must tee to ./log/)
exec > >(tee -a "$LOG_FILE") 2>&1

PASS=0
FAIL=0

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log_status() {
	local exit_code=$1
	local name=$2
	local detail=$3
	if [[ $exit_code -eq 0 ]]; then
		echo -e "${GREEN}  ✅ PASSED${NC}: ${name}"
		PASS=$((PASS + 1))
	else
		echo -e "${RED}  ❌ FAILED${NC}: ${name}"
		if [[ -n "$detail" ]]; then
			echo -e "${YELLOW}    ${detail}${NC}"
		fi
		FAIL=$((FAIL + 1))
	fi
}

main() {
	echo -e "${CYAN}=== commit-hook: ai-mirror ===${NC}"
	echo ""

	# ============================================
	# Phase 0: Version check
	# ============================================
	echo -e "${CYAN}--- Phase 0: Version Check ---${NC}"

	local VERSION_FILE="$PROJECT_DIR/include/ai_mirror/version.hpp.in"
	if [[ -f "$VERSION_FILE" ]]; then
		# Check if version file changed in staged files
		if git diff --cached --name-only | grep -q "version.hpp.in"; then
			log_status 0 "version update" "version.hpp.in changed in commit"
		else
			# Check if any source files changed (excluding version file itself)
			local CHANGED
			CHANGED=$(git diff --cached --name-only | grep -E "^(src/|include/)" | grep -v "version.hpp" | head -1)
			if [[ -n "$CHANGED" ]]; then
				log_status 1 "version update required" "source changed but version.hpp.in not updated"
				echo -e "${YELLOW}    Files changed: ${CHANGED}${NC}"
				echo -e "${YELLOW}    Action: edit version.hpp.in (update comment timestamp or add changelog)${NC}"
			else
				log_status 0 "version check" "no source changes"
			fi
		fi
	else
		log_status 1 "version file missing" "include/ai_mirror/version.hpp.in not found"
	fi

	# ============================================
	# Phase 1: Code check (clang-format dry-run)
	# ============================================
	echo -e "${CYAN}--- Phase 1: Code Check ---${NC}"

	if ! command -v clang-format &>/dev/null; then
		echo -e "${YELLOW}  ⚠️  SKIPPED: clang-format not installed${NC}"
	else
		local -a FAILED_FILES=()
		# Find C++ source files
		while IFS= read -r -d '' f; do
			if ! clang-format --dry-run -Werror "$f" 2>/dev/null; then
				FAILED_FILES+=("$f")
			fi
		done < <(find "$PROJECT_DIR/src" "$PROJECT_DIR/include" \
			\( -name "*.cpp" -o -name "*.hpp" -o -name "*.h" \) -print0 2>/dev/null || true)

		if [[ ${#FAILED_FILES[@]} -eq 0 ]]; then
			log_status 0 "clang-format check" ""
		else
			log_status 1 "clang-format check" "${#FAILED_FILES[@]} files need formatting"
			for f in "${FAILED_FILES[@]}"; do
				echo -e "${YELLOW}      $f${NC}"
			done
			echo -e "${YELLOW}    Run: clang-format -i <files>${NC}"
		fi
	fi

	# ============================================
	# Phase 1b: Source file permissions check
	# ============================================
	echo -e "${CYAN}--- Phase 1b: File Permissions Check ---${NC}"

	local BAD_PERMS=0
	while IFS= read -r -d '' f; do
		local perms
		perms=$(stat -c '%a' "$f")
		if [[ "$perms" != "600" ]]; then
			echo -e "${YELLOW}      $f ($perms)${NC}"
			BAD_PERMS=$((BAD_PERMS + 1))
		fi
	done < <(find "$PROJECT_DIR/src" "$PROJECT_DIR/include" "$PROJECT_DIR/tests" "$PROJECT_DIR/scripts" \
		\( -name "*.cpp" -o -name "*.hpp" -o -name "*.h" -o -name "*.cxx" -o -name "*.py" -o -name "*.sh" \) -print0 2>/dev/null)

	if [[ $BAD_PERMS -eq 0 ]]; then
		log_status 0 "file permissions (600)" ""
	else
		log_status 1 "file permissions (600)" "$BAD_PERMS files need chmod 600"
		echo -e "${YELLOW}    Fix: find src/ include/ tests/ scripts/ -type f \\( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.py' -o -name '*.sh' \\) -exec chmod 600 {} +${NC}"
	fi

	# ============================================
	# Phase 2: Build verification
	# ============================================
	echo ""
	echo -e "${CYAN}--- Phase 2: Build Verification ---${NC}"

	local BUILD_DIR="$PROJECT_DIR/build-test"
	if [[ -f "$PROJECT_DIR/CMakeLists.txt" ]]; then
		set +e
		local BUILD_OUTPUT BUILD_EXIT
		BUILD_OUTPUT=$(cmake --build "$BUILD_DIR" --target ai-mirror -j4 2>&1)
		BUILD_EXIT=$?
		set -e

		if [[ $BUILD_EXIT -eq 0 ]]; then
			log_status 0 "cmake build" ""
		else
			log_status 1 "cmake build" "see errors above"
			echo "$BUILD_OUTPUT" | tail -20
		fi
	else
		echo -e "${YELLOW}  ⚠️  SKIPPED: CMakeLists.txt not found${NC}"
	fi

	# ============================================
	# Phase 3: Unit tests (Docker)
	# ============================================
	echo ""
	echo -e "${CYAN}--- Phase 3: Unit Tests (Docker) ---${NC}"

	if command -v docker &>/dev/null; then
		local test_exit=0
		# Build and run tests inside Docker container (--privileged for mount/ssh/useradd)
		docker build -t ai-mirror-test -f "$PROJECT_DIR/tests/Dockerfile.test" "$PROJECT_DIR" >/dev/null 2>&1 || test_exit=$?
		if [[ $test_exit -eq 0 ]]; then
			docker run --rm --privileged ai-mirror-test || test_exit=$?
		fi

		if [[ $test_exit -eq 0 ]]; then
			log_status 0 "unit tests (Docker)" ""
		else
			log_status 1 "unit tests (Docker)" "Docker build or test failed (exit $test_exit)"
		fi
	else
		echo -e "${YELLOW}  ⚠️  SKIPPED: docker not installed (required for rootless testing)${NC}"
	fi

	# ============================================
	# Summary
	# ============================================
	echo ""
	echo -e "${CYAN}=== Summary ===${NC}"
	echo -e "  Passed: ${GREEN}${PASS}${NC}"
	echo -e "  Failed: ${RED}${FAIL}${NC}"
	echo ""

	if [[ $FAIL -gt 0 ]]; then
		echo -e "${RED}commit blocked: ${FAIL} checks failed${NC}"
		exit 1
	fi

	echo -e "${GREEN}commit allowed: all checks passed${NC}"
	exit 0
}

main "$@"
