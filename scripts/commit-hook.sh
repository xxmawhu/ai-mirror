#!/usr/bin/env bash
#
# ai-mirror commit-hook: four-phase validation
# Phase 0: Version check (version.hpp.in must be updated)
# Phase 1: Code check (clang-format dry-run)
# Phase 2: Build verification (cmake)
# Phase 3: Unit tests (Docker, no host root required)
#
# [git-tidy] 屏幕简洁 / 日志详尽，严格分离
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
LOG_DIR="$PROJECT_DIR/log/hook"
mkdir -p "$LOG_DIR"
HOOK_NAME="commit-hook"
LOG_FILE="$LOG_DIR/${HOOK_NAME}.$(date +%Y%m%d).log"

# 详尽日志：写日志文件，不打扰屏幕
log_file() {
	local level="$1"
	shift
	local stage="$1"
	shift
	echo "[$(date '+%Y-%m-%d %H:%M:%S')] [$level] [stage:$stage] $*" >>"$LOG_FILE"
}

# 极简屏幕输出
screen_out() {
	local emoji="$1"
	shift
	local stage="$1"
	shift
	if [ "$emoji" = "❌" ]; then
		echo "$emoji $stage（详见 log/hook/$(basename "$LOG_FILE")）"
	else
		echo "$emoji $stage"
	fi
}

# 阶段执行器
run_stage() {
	local stage="$1"
	shift
	local start
	start=$(date +%s)
	screen_out "🔍" "$stage..."
	log_file INFO "$stage" "start"
	if "$@" >>"$LOG_FILE" 2>&1; then
		local elapsed
		elapsed=$(($(date +%s) - start))
		log_file PASS "$stage" "elapsed=${elapsed}s"
		screen_out "✅" "$stage"
		return 0
	else
		local rc=$?
		local elapsed
		elapsed=$(($(date +%s) - start))
		log_file FAIL "$stage" "exit_code=$rc, elapsed=${elapsed}s"
		log_file ERROR "$stage" "command: $*"
		screen_out "❌" "$stage"
		return $rc
	fi
}

main() {
	# 记录触发上下文
	local branch
	branch=$(git -C "$PROJECT_DIR" rev-parse --abbrev-ref HEAD 2>/dev/null || echo 'unknown')
	local commit
	commit=$(git -C "$PROJECT_DIR" rev-parse --short HEAD 2>/dev/null || echo 'unknown')
	log_file INFO init "$HOOK_NAME triggered by user=$(whoami), branch=$branch, commit=$commit"
	screen_out "🔍" "$HOOK_NAME [$branch@$commit]"

	local fail=0

	# ============================================
	# Phase 0: Version check
	# ============================================
	local version_file="$PROJECT_DIR/include/ai_mirror/version.hpp.in"
	if [[ -f "$version_file" ]]; then
		if git diff --cached --name-only | grep -q "version.hpp.in"; then
			log_file PASS "version" "version.hpp.in changed in commit"
			screen_out "✅" "version check"
		else
			local changed
			changed=$(git diff --cached --name-only | grep -E "^(src/|include/)" | grep -v "version.hpp" | head -1)
			if [[ -n "$changed" ]]; then
				log_file FAIL "version" "source changed but version.hpp.in not updated: $changed"
				screen_out "❌" "version check"
				fail=1
			else
				log_file PASS "version" "no source changes"
				screen_out "✅" "version check"
			fi
		fi
	else
		log_file FAIL "version" "include/ai_mirror/version.hpp.in not found"
		screen_out "❌" "version check"
		fail=1
	fi

	# ============================================
	# Phase 1: Code check (clang-format)
	# ============================================
	if command -v clang-format &>/dev/null; then
		if run_stage "clang-format" bash -c "
			fail_inner=0
			while IFS= read -r -d '' f; do
				if ! clang-format --dry-run -Werror \"\$f\" 2>/dev/null; then
					echo \"needs formatting: \$f\"
					fail_inner=1
				fi
			done < <(find \"$PROJECT_DIR/src\" \"$PROJECT_DIR/include\" \
				\\( -name \"*.cpp\" -o -name \"*.hpp\" -o -name \"*.h\" \\) -print0 2>/dev/null || true)
			exit \$fail_inner
		"; then
			:
		else
			fail=1
		fi
	else
		log_file WARN "clang-format" "clang-format not installed, skipping"
		screen_out "⏭️" "clang-format"
	fi

	# ============================================
	# Phase 2: Build verification
	# ============================================
	local build_dir="$PROJECT_DIR/build-test"
	if [[ -f "$PROJECT_DIR/CMakeLists.txt" ]]; then
		if run_stage "build" cmake --build "$build_dir" --target ai-mirror -j4; then
			:
		else
			fail=1
		fi
	else
		log_file WARN "build" "CMakeLists.txt not found"
		screen_out "⏭️" "build"
	fi

	# ============================================
	# Phase 3: Unit tests (Docker)
	# ============================================
	if command -v docker &>/dev/null; then
		if run_stage "tests" bash -c "
			docker build -t ai-mirror-test -f \"$PROJECT_DIR/tests/Dockerfile.test\" \"$PROJECT_DIR\" > /dev/null 2>&1 && \
			docker run --rm --privileged ai-mirror-test
		"; then
			:
		else
			fail=1
		fi
	else
		log_file WARN "tests" "docker not installed, skipping"
		screen_out "⏭️" "tests"
	fi

	# ============================================
	# Summary
	# ============================================
	if [[ $fail -gt 0 ]]; then
		screen_out "❌" "$HOOK_NAME failed"
		log_file FAIL "done" "$HOOK_NAME blocked: $fail check(s) failed"
		exit 1
	fi

	screen_out "✅" "$HOOK_NAME done"
	log_file PASS "done" "$HOOK_NAME completed successfully"
}

main "$@"
