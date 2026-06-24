#!/usr/bin/env bash
# ai-mirror pre-commit installation script
# Single entry point for pre-commit setup (git-tidy Rule 8)
#
# [git-tidy] 屏幕简洁 / 日志详尽，严格分离
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
LOG_DIR="$PROJECT_DIR/log/hook"
mkdir -p "$LOG_DIR"
HOOK_NAME="setup-pre-commit"
LOG_FILE="$LOG_DIR/${HOOK_NAME}.$(date +%Y%m%d).log"

# 详尽日志
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

	# Step 1: Check / install pre-commit
	if command -v pre-commit &>/dev/null; then
		local pc_version
		pc_version=$(pre-commit --version 2>&1)
		log_file PASS "install" "pre-commit already installed: $pc_version"
		screen_out "✅" "pre-commit found"
	else
		log_file INFO "install" "pre-commit not found, installing..."
		screen_out "🔍" "installing pre-commit..."
		if command -v pipx &>/dev/null; then
			# pipx is the recommended installer for Python apps (PEP 668 safe)
			pipx install pre-commit >>"$LOG_FILE" 2>&1
		elif command -v pip3 &>/dev/null; then
			pip3 install --user pre-commit >>"$LOG_FILE" 2>&1
		elif command -v pip &>/dev/null; then
			pip install --user pre-commit >>"$LOG_FILE" 2>&1
		else
			log_file ERROR "install" "pipx/pip3/pip not found"
			screen_out "❌" "install pre-commit"
			exit 1
		fi

		# After installation, ensure pre-commit is on PATH
		if ! command -v pre-commit &>/dev/null; then
			local pipx_bin_dir
			pipx_bin_dir=$(pipx environment 2>/dev/null | grep PIPX_BIN_DIR | cut -d= -f2-)
			if [ -n "$pipx_bin_dir" ] && [ -f "$pipx_bin_dir/pre-commit" ]; then
				export PATH="$pipx_bin_dir:$PATH"
				log_file INFO "path" "added $pipx_bin_dir to PATH"
			elif [ -f "$HOME/.local/bin/pre-commit" ]; then
				export PATH="$HOME/.local/bin:$PATH"
				log_file INFO "path" "added $HOME/.local/bin to PATH"
			else
				log_file ERROR "path" "pre-commit installed but not found on PATH"
				screen_out "❌" "pre-commit not found after install"
				exit 1
			fi
		fi
		log_file PASS "install" "pre-commit installed via pipx/pip"
		screen_out "✅" "pre-commit installed"
	fi

	# Step 2: Install pre-commit hooks
	cd "$PROJECT_DIR"

	if [ ! -f ".pre-commit-config.yaml" ]; then
		log_file ERROR "config" ".pre-commit-config.yaml not found"
		screen_out "❌" "pre-commit config"
		exit 1
	fi

	run_stage "install: pre-commit" pre-commit install || true
	run_stage "install: post-merge" pre-commit install --hook-type post-merge || true

	screen_out "✅" "$HOOK_NAME done"
	log_file PASS "done" "$HOOK_NAME completed successfully"
}

main "$@"
