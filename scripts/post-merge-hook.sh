#!/usr/bin/env bash
# ai-mirror post-merge hook: auto-install after git pull
# Managed by pre-commit framework, configured in .pre-commit-config.yaml
#
# 自动 issue 反馈机制（release 部署场景）：
# 当检测到运行在 ~/release/ 下时，任何安装失败或异常都会自动生成 issue
# 并通过 raise-issue 发送到开发项目（~/dev/aimirror/ai-mirror/issues/）
#
# [git-tidy] 屏幕简洁 / 日志详尽，严格分离
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
LOG_DIR="$PROJECT_DIR/log/hook"
mkdir -p "$LOG_DIR"
HOOK_NAME="post-merge"
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

# ---- Issue reporting helpers ----

# Detect release deployment context
is_release_deploy() {
	case "$PROJECT_DIR" in
	"$HOME/release"/*) return 0 ;;
	*/maxx/release/*) return 0 ;;
	*) return 1 ;;
	esac
}

dev_project_path() {
	local dir="$PROJECT_DIR"
	echo "${dir/\/release\//\/dev\/aimirror\/}"
}

report_issue_to_dev() {
	local phase="$1"
	local detail="$2"

	if ! is_release_deploy; then
		log_file INFO "report" "dev context, skipping issue report"
		return 0
	fi

	local dev_proj
	dev_proj=$(dev_project_path)
	if [[ ! -d "$dev_proj/issues" ]]; then
		log_file WARN "report" "dev project issues/ not found at $dev_proj"
		return 0
	fi

	local raise_issue_bin="$HOME/.local/bin/raise-issue"
	if [[ ! -x "$raise_issue_bin" ]]; then
		log_file WARN "report" "raise-issue not found"
		return 0
	fi

	local tmp_issues="$PROJECT_DIR/tmp-issues"
	mkdir -p "$tmp_issues"
	local ts
	ts=$(date '+%Y-%m-%d-%H%M%S')
	local issue_file="$tmp_issues/post-merge-fail-${ts}.md"

	cat >"$issue_file" <<ISSUE_EOF
# Issue: post-merge 部署失败 — ${phase}

**日期**: $(date '+%Y-%m-%d %H:%M:%S')
**来源**: release 目录 post-merge hook 自动报告
**主机**: $(hostname 2>/dev/null || echo unknown)
**路径**: ${PROJECT_DIR}
**严重程度**: P1 — 部署阻塞

## 失败阶段

${phase}

## 错误详情

\`\`\`
${detail}
\`\`\`

## 日志

完整日志见: ${LOG_FILE}

## 要求

请检查并修复部署失败问题。
ISSUE_EOF

	# redirect stderr to stdout first, then append both to log
	"$raise_issue_bin" "$issue_file" "$dev_proj/issues/" >>"$LOG_FILE" 2>&1 || true
	log_file INFO "report" "Issue auto-submitted to dev project: $dev_proj"
	return 0
}

# ---- Main ----

main() {
	cd "$PROJECT_DIR"

	# 记录触发上下文
	local branch
	branch=$(git -C "$PROJECT_DIR" rev-parse --abbrev-ref HEAD 2>/dev/null || echo 'unknown')
	local commit
	commit=$(git -C "$PROJECT_DIR" rev-parse --short HEAD 2>/dev/null || echo 'unknown')
	log_file INFO init "$HOOK_NAME triggered by user=$(whoami), branch=$branch, commit=$commit"
	screen_out "🔍" "$HOOK_NAME [$branch@$commit]"

	local -a deploy_errors=()

	# Sync submodules
	if run_stage "git submodule update" git submodule update --init --recursive; then
		:
	else
		deploy_errors+=("git submodule update failed")
	fi

	# Re-run setup-hooks
	if [[ -f "scripts/setup-hooks.sh" ]]; then
		if run_stage "setup hooks" bash scripts/setup-hooks.sh; then
			:
		else
			deploy_errors+=("setup hooks failed")
		fi
	fi

	# Build and install
	if run_stage "build & install" bash install.sh; then
		:
	else
		deploy_errors+=("install.sh failed")
	fi

	if [[ ${#deploy_errors[@]} -gt 0 ]]; then
		local all_errors
		all_errors=$(printf '%s\n' "${deploy_errors[@]}")
		log_file ERROR "deploy" "$all_errors"
		report_issue_to_dev "post-merge 部署" "$all_errors"
		screen_out "❌" "$HOOK_NAME done (${#deploy_errors[@]} error(s))"
		log_file FAIL "done" "$HOOK_NAME completed with ${#deploy_errors[@]} error(s)"
	else
		screen_out "✅" "$HOOK_NAME done"
		log_file PASS "done" "$HOOK_NAME completed successfully"
	fi
}

# post-merge hook must never block git pull
main "$@" || {
	report_issue_to_dev "post-merge hook 异常退出" "main() 异常退出 (exit=$?)"
	exit 0
}
exit 0
