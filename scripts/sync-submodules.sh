#!/usr/bin/env bash
#
# sync-submodules.sh - 同步最小文件集到子模块（root.md Section 9）
#
# 设计原则：
# - 白名单制：只同步明确列出的文件/目录
# - 编译验证：同步后验证子模块可独立编译
# - 最小暴露：子模块文件数应远小于主仓
#
# Usage:
#   bash scripts/sync-submodules.sh [OPTIONS]
#
# Options:
#   --dry-run     仅显示要同步的文件，不执行
#   --no-push     同步但不推送到远程
#   --no-verify   跳过编译验证
#   --force       强制重新同步所有必需文件
#   --clean       清理子模块中不属于白名单的文件
#
set -euo pipefail

# ---- Configuration ----
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MAIN_REPO="$(dirname "$SCRIPT_DIR")"
SUBMODULES=("gitee-ai-mirror" "gitlib-ai-mirror" "github-ai-mirror")

# ---- 白名单：编译/安装必需的文件和目录 ----
# 严格最小化：仅包含编译和安装必需的文件
# 禁止推送：tests/, llm-docs/, docker-test/, profile/, scripts/, docs/, memory/, *-ai-mirror/

# 目录（rsync 会递归同步目录下所有文件）
SYNC_DIRS=(
  "src"
  "include"
  "completions"
)

# 单文件
SYNC_FILES=(
  "CMakeLists.txt"
  "install.sh"
  "README.md"
  ".gitignore"
)

# 可选文件（存在则同步）—— 已移除所有测试文件
SYNC_OPTIONAL=()

# ---- Colors ----
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
NC='\033[0m'

# ---- Helpers ----
log() { echo -e "${GREEN}[sync]${NC} $*"; }
warn() { echo -e "${YELLOW}[warn]${NC} $*"; }
error() { echo -e "${RED}[error]${NC} $*" >&2; }
info() { echo -e "${CYAN}[info]${NC} $*"; }

# ---- Options ----
DRY_RUN=false
NO_PUSH=false
NO_VERIFY=false
FORCE=false
CLEAN=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run)   DRY_RUN=true; shift ;;
    --no-push)   NO_PUSH=true; shift ;;
    --no-verify) NO_VERIFY=true; shift ;;
    --force)     FORCE=true; shift ;;
    --clean)     CLEAN=true; shift ;;
    -h|--help)
      echo "Usage: bash scripts/sync-submodules.sh [--dry-run] [--no-push] [--no-verify] [--force] [--clean]"
      exit 0
      ;;
    *) error "Unknown option: $1"; exit 1 ;;
  esac
done

# ---- Functions ----

# 统计文件数量（排除 .git 和 build 目录）
count_files() {
  local repo="$1"
  cd "$repo"
  find . -type f \
    -not -path './.git/*' \
    -not -path './build-*/*' \
    -not -path './_deps/*' \
    -not -path './.cache/*' \
    -not -path './log/*' \
    -not -path './data/*' | wc -l
}

# 列出子模块中应该存在的白名单文件
list_allowed_files() {
  local submodule="$1"
  cd "$submodule"

  # 白名单目录下的文件
  for dir in "${SYNC_DIRS[@]}"; do
    if [[ -d "$dir" ]]; then
      find "$dir" -type f
    fi
  done

  # 白名单单文件
  for file in "${SYNC_FILES[@]}"; do
    if [[ -f "$file" ]]; then
      echo "$file"
    fi
  done

  # 白名单可选文件
  for file in "${SYNC_OPTIONAL[@]}"; do
    if [[ -f "$file" ]]; then
      echo "$file"
    fi
  done

  # .gitignore 始终保留
  echo ".gitignore"
}

# 清理子模块中的无关文件
clean_submodule() {
  local submodule="$1"
  cd "$MAIN_REPO/$submodule"

  log "Cleaning non-whitelisted files in $submodule..."

  # 强制清理列表（安装无关，必须删除）
  local FORCE_REMOVE=(
    "tests"
    "llm-docs"
    "docker-test"
    "docker"
    "docs"
    "profile"
    "scripts"
    "memory"
    "issues"
    "plan"
    "end_task"
    "log"
    "data"
    ".cache"
    "publish-llm-docs.sh"
    "release.sh"
    "gitee-ai-mirror"
    "github-ai-mirror"
    "gitlib-ai-mirror"
  )

  # 强制删除目录和文件
  local removed=0
  for item in "${FORCE_REMOVE[@]}"; do
    if [[ -e "$item" ]]; then
      warn "  Removing (force): $item"
      $DRY_RUN || { git rm -rf --cached "$item" 2>/dev/null || rm -rf "$item"; }
      removed=$((removed + 1))
    fi
  done

  # 构建允许保留的文件列表
  local allowed_tmp
  allowed_tmp=$(mktemp)
  list_allowed_files "." > "$allowed_tmp"

  # 查找不在白名单中的 tracked 文件
  while IFS= read -r file; do
    # 跳过 .gitignore 和 .git 目录
    [[ "$file" == ".gitignore" ]] && continue
    [[ "$file" == .git/* ]] && continue
    # 跳过强制清理列表中的文件（已处理）
    for item in "${FORCE_REMOVE[@]}"; do
      [[ "$file" == "$item" ]] && continue 2
      [[ "$file" == ${item}/* ]] && continue 2
    done

    if ! grep -qx "$file" "$allowed_tmp" 2>/dev/null; then
      if [[ -f "$file" ]]; then
        warn "  Removing: $file"
        $DRY_RUN || git rm --cached -f "$file" 2>/dev/null || rm -f "$file"
        removed=$((removed + 1))
      fi
    fi
  done < <(git ls-files 2>/dev/null || find . -type f -not -path './.git/*')

  rm -f "$allowed_tmp"
  info "  Removed $removed non-whitelisted files"
}

# 同步文件到子模块
sync_to_submodule() {
  local submodule="$1"
  cd "$MAIN_REPO"

  local synced=0

  # 同步目录
  for dir in "${SYNC_DIRS[@]}"; do
    if [[ -d "$dir" ]]; then
      info "  Syncing dir: $dir/"
      if ! $DRY_RUN; then
        mkdir -p "$submodule/$dir"
        # rsync: 只同步源码/头文件，排除构建产物和嵌套子模块
        rsync -a --delete \
          --include='*.cpp' --include='*.hpp' --include='*.h' --include='*.hpp.in' --include='*.in' \
          --include='*.bash' \
          --exclude='*' \
          --exclude='gitee-ai-mirror' --exclude='github-ai-mirror' --exclude='gitlib-ai-mirror' \
          "$dir/" "$submodule/$dir/"
      fi
      synced=$((synced + 1))
    fi
  done

  # 同步单文件
  for file in "${SYNC_FILES[@]}"; do
    if [[ -f "$file" ]]; then
      info "  Syncing: $file"
      if [[ "$file" == "CMakeLists.txt" ]]; then
        # CMakeLists.txt: 移除测试配置，只保留编译
        $DRY_RUN || {
          # 删除所有测试相关的行（enable_testing, add_test, tests/ 目录配置）
          # 使用 Python 脚本处理 if/endif 嵌套
          "${MAIN_REPO}/scripts/fix_cmake_for_submodule.py" "$submodule/$file"
        }
      else
        $DRY_RUN || cp "$file" "$submodule/$file"
      fi
      synced=$((synced + 1))
    fi
  done

  # 同步可选文件
  for file in "${SYNC_OPTIONAL[@]}"; do
    if [[ -f "$file" ]]; then
      info "  Syncing: $file (optional)"
      $DRY_RUN || { mkdir -p "$submodule/$(dirname "$file")"; cp "$file" "$submodule/$file"; }
      synced=$((synced + 1))
    fi
  done

  # 设置文件权限
  if ! $DRY_RUN; then
    cd "$MAIN_REPO/$submodule"
    find src include -type f -exec chmod 600 {} \; 2>/dev/null || true
    find completions -type f -exec chmod 600 {} \; 2>/dev/null || true
    [[ -f install.sh ]] && chmod 700 install.sh
  fi

  return 0
}

# 验证子模块可编译
verify_submodule() {
  local submodule="$1"

  if $NO_VERIFY; then
    warn "Skipping verification (--no-verify)"
    return 0
  fi

  log "Verifying $submodule build..."

  cd "$MAIN_REPO/$submodule"

  # 清理旧构建
  rm -rf build-sync-verify 2>/dev/null || true

  # CMake 配置
  if ! cmake -B build-sync-verify -S . -DCMAKE_BUILD_TYPE=Debug 2>&1; then
    error "CMake configure failed"
    rm -rf build-sync-verify
    return 1
  fi

  # 编译两个 target
  if ! cmake --build build-sync-verify --target am ai-mirror-bin -j4 2>&1; then
    error "Build failed"
    rm -rf build-sync-verify
    return 1
  fi

  # 验证二进制存在
  local ok=true
  [[ -f build-sync-verify/bin/am ]] || { error "bin/am not found"; ok=false; }
  [[ -f build-sync-verify/bin/ai-mirror-bin ]] || { error "bin/ai-mirror-bin not found"; ok=false; }

  # 清理
  rm -rf build-sync-verify

  if $ok; then
    log "Verification PASSED"
    return 0
  else
    return 1
  fi
}

# ---- Main ----

log "=== Submodule Sync (Minimal Exposure, root.md Section 9) ==="
log "Main repo: $MAIN_REPO"

cd "$MAIN_REPO"

# 统计主仓
MAIN_COUNT=$(count_files "$MAIN_REPO")
info "Main repo: $MAIN_COUNT files"

for submodule in "${SUBMODULES[@]}"; do
  echo ""
  log "--- $submodule ---"

  if [[ ! -d "$submodule" ]]; then
    error "Not found: $submodule"
    continue
  fi

  # 统计子模块当前文件
  BEFORE=$(count_files "$MAIN_REPO/$submodule")
  info "Before: $BEFORE files"

  # Step 1: 清理无关文件（如果指定 --clean 或 --force）
  if $CLEAN || $FORCE; then
    clean_submodule "$submodule"
  fi

  # Step 2: 同步白名单文件
  sync_to_submodule "$submodule"

  # Step 3: 统计同步后文件
  AFTER=$(count_files "$MAIN_REPO/$submodule")
  info "After: $AFTER files (delta: $((AFTER - BEFORE)))"

  if $DRY_RUN; then
    warn "Dry-run: skipping verify/commit/push"
    continue
  fi

  # Step 4: 验证编译
  if ! verify_submodule "$submodule"; then
    error "Verification FAILED for $submodule — skipping commit"
    continue
  fi

  # Step 5: 检查变更
  cd "$MAIN_REPO/$submodule"
  if git diff --quiet && git diff --cached --quiet 2>/dev/null; then
    info "No changes — skipping commit"
    cd "$MAIN_REPO"
    continue
  fi

  # Step 6: Commit
  git add -A
  git commit -m "sync: minimal exposure ($AFTER files, root.md Section 9)"
  log "Committed"

  # Step 7: Push
  if $NO_PUSH; then
    warn "Skipping push (--no-push)"
  else
    log "Pushing..."
    if ! git push 2>&1; then
      error "Push failed — may need pull/rebase"
    fi
  fi

  cd "$MAIN_REPO"
done

echo ""
log "=== Done ==="

# 更新主仓子模块引用
if ! $DRY_RUN && ! $NO_PUSH; then
  cd "$MAIN_REPO"
  if ! git diff --quiet gitee-ai-mirror gitlib-ai-mirror github-ai-mirror 2>/dev/null; then
    git add gitee-ai-mirror gitlib-ai-mirror github-ai-mirror
    git commit -m "chore: update submodule refs after sync" || true
    log "Updated submodule references in main repo"
  fi
fi
