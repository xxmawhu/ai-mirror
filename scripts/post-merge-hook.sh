#!/usr/bin/env bash
# ai-mirror post-merge hook: auto-install after git pull
# Managed by pre-commit framework, configured in .pre-commit-config.yaml
# [log-review] 日志输出到 ./log/hook/ (Rule 2/9)
#
# 自动 issue 反馈机制（release 部署场景）：
# 当检测到运行在 ~/release/ 下时，任何安装失败或异常都会自动生成 issue
# 并通过 raise-issue 发送到开发项目（~/dev/aimirror/ai-mirror/issues/），
# 确保部署问题能被开发项目自动发现和处理。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
LOG_DIR="$PROJECT_DIR/log/hook"
LOG_FILE="$LOG_DIR/post-merge-$(date +%Y-%m-%d).log"

mkdir -p "$LOG_DIR"
exec > >(tee -a "$LOG_FILE") 2>&1

# ---- Issue reporting helpers ----

# Detect release deployment context: ~maxx/release (production checkout).
# Matches $HOME/release (whoever runs git pull) and ~maxx/release explicitly,
# so other unrelated /release/ paths on the system are not mistaken for the
# ai-mirror production deploy.
is_release_deploy() {
	case "$PROJECT_DIR" in
	"$HOME/release"/*) return 0 ;;        # e.g. ~maxx/release/ai-mirror
	*/maxx/release/*) return 0 ;;         # explicit ~maxx/release fallback
	*) return 1 ;;
	esac
}

# Derive dev project path from release path.
# ~maxx/release/ai-mirror -> ~maxx/dev/aimirror/ai-mirror
dev_project_path() {
	echo "$PROJECT_DIR" | sed 's|/release/|/dev/aimirror/|'
}

# Report a deployment failure to the dev project via raise-issue.
# Only fires in release context; in dev context failures just log.
# Always returns 0 so it never blocks git pull.
report_issue_to_dev() {
	local phase="$1"
	local detail="$2"

	if ! is_release_deploy; then
		echo "[post-merge] dev context, skipping issue report"
		return 0
	fi

	local dev_proj
	dev_proj=$(dev_project_path)
	if [[ ! -d "$dev_proj/issues" ]]; then
		echo "[post-merge] WARN: dev project issues/ not found at $dev_proj"
		return 0
	fi

	local raise_issue_bin="$HOME/.local/bin/raise-issue"
	if [[ ! -x "$raise_issue_bin" ]]; then
		echo "[post-merge] WARN: raise-issue not found at $raise_issue_bin"
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

请检查并修复部署失败问题。这是 release 目录 git pull 后自动触发的
post-merge hook 部署流程，失败意味着用户无法获得最新版本。
ISSUE_EOF

	if "$raise_issue_bin" "$issue_file" "$dev_proj/issues/" 2>&1; then
		echo "[post-merge] Issue 已自动提交到开发项目: $dev_proj"
	else
		echo "[post-merge] WARN: raise-issue 失败，issue 保存在: $issue_file"
	fi
	return 0
}

# ---- Main ----

main() {
	echo "=== post-merge: deploying ai-mirror ==="
	cd "$PROJECT_DIR"

	local deploy_errors=()

	# Sync submodules to the commit recorded in the main repo.
	# Without this, `git pull` advances the main repo HEAD (and thus the
	# submodule gitlink pointers) but leaves the submodule working trees at
	# their old commits — `git status` then shows "modified: <submodule>
	# (new commits)" and the repo appears dirty. `--init` handles a freshly
	# cloned release checkout; `--recursive` covers nested submodules.
	# [bash-code-review Rule 39] sudo not needed: submodules are user-writable.
	local submodule_output
	if ! submodule_output=$(git submodule update --init --recursive 2>&1); then
		deploy_errors+=("git submodule update 失败: ${submodule_output}")
	fi

	# Re-run setup-hooks to ensure hooks are up-to-date
	if [[ -f "scripts/setup-hooks.sh" ]]; then
		bash scripts/setup-hooks.sh 2>&1 || true
	fi

	# Build and install ai-mirror.
	# Capture output for issue reporting; do NOT let failure block git pull.
	local install_output
	if ! install_output=$(bash install.sh 2>&1); then
		deploy_errors+=("install.sh 失败:\n${install_output}")
	fi

	if [[ ${#deploy_errors[@]} -gt 0 ]]; then
		local all_errors
		all_errors=$(printf '%b\n' "${deploy_errors[@]}")
		echo "[post-merge] 部署遇到 ${#deploy_errors[@]} 个错误:"
		echo "$all_errors"
		report_issue_to_dev "post-merge 部署" "$all_errors"
		echo "=== deploy completed with errors (issue reported) ==="
	else
		echo "=== deploy complete ==="
	fi
}

# post-merge hook must never block git pull — catch any unexpected error
main "$@" || {
	report_issue_to_dev "post-merge hook 异常退出" "main() 异常退出 (exit=$?)"
	exit 0
}
exit 0
