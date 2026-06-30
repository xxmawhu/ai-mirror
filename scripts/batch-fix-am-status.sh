#!/usr/bin/env bash
# batch-fix-am-status.sh — 批量修复 46 个项目的 .am_status source 路径错误
#
# 背景: am 工具之前写入 .am_status 时 source 路径使用了 /usr/maxx/（不存在），
# 而非正确的 /mnt/beegfs_data/usr/maxx/。代码已修复（commits 0b038fd, 55f3f92, cdceba1），
# 但存量 .am_status 文件需要 am update 重新生成。
#
# 方法: 遍历所有含 /usr/maxx/ 错误的 .am_status 项目，为每个执行 am update
#
# 用法: sudo -u root bash scripts/batch-fix-am-status.sh
#       （am update 需要 root 权限执行 mount 操作）

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DEV_ROOT="/mnt/beegfs_data/usr/maxx/dev"
LOG_FILE="$PROJECT_ROOT/log/fix/batch-fix-am-status-$(date +%Y%m%d-%H%M%S).log"
WRONG_PREFIX="/usr/maxx/"

# Ensure log directory exists
mkdir -p "$(dirname "$LOG_FILE")"

log() {
	local level="$1"
	local msg="$2"
	echo "[$(date '+%Y-%m-%d %H:%M:%S')] [$level] $msg" | tee -a "$LOG_FILE"
}

log "INFO" "Starting batch fix of .am_status source paths"
log "INFO" "Scanning for projects with wrong source prefix: $WRONG_PREFIX"

# Find all .am_status files with wrong source paths
broken_projects=()
while IFS= read -r status_file; do
	project_dir="$(dirname "$status_file")"
	broken_projects+=("$project_dir")
done < <(fdfind -H -t f '.am_status' "$DEV_ROOT" 2>/dev/null | xargs grep -l "$WRONG_PREFIX" 2>/dev/null || true)

total="${#broken_projects[@]}"
log "INFO" "Found $total projects with broken .am_status"

if [ "$total" -eq 0 ]; then
	log "INFO" "No broken projects found. Nothing to do."
	exit 0
fi

# Run am update on each broken project
success=0
failed=0
for ((i = 0; i < total; i++)); do
	project="${broken_projects[$i]}"
	idx=$((i + 1))
	log "INFO" "[$idx/$total] Running am update on: $project"

	set +e
	output=$(am update "$project" 2>&1)
	rc=$?
	set -e

	if [ $rc -eq 0 ]; then
		log "OK" "[$idx/$total] am update succeeded: $project"
		success=$((success + 1))
	else
		log "WARN" "[$idx/$total] am update failed (rc=$rc): $project — $output"
		failed=$((failed + 1))
	fi
done

log "INFO" "=== Batch fix complete ==="
log "INFO" "Total: $total, Success: $success, Failed: $failed"

# Verify fix: scan for remaining wrong paths
log "INFO" "Verifying fix — scanning for remaining /usr/maxx/ paths..."
remaining=$(fdfind -H -t f '.am_status' "$DEV_ROOT" 2>/dev/null | xargs grep -l "$WRONG_PREFIX" 2>/dev/null | wc -l || echo 0)
log "INFO" "Remaining broken source paths: $remaining"

if [ "$remaining" -eq 0 ]; then
	log "INFO" "✅ All source paths fixed correctly!"
else
	log "WARN" "⚠️  $remaining source paths still broken, may need manual fix"
fi

exit "$failed"
