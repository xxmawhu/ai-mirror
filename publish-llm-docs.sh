#!/usr/bin/env bash
# ai-mirror llm-docs 发布脚本
# 将文档镜像同步到 ~/.local/share/llm-docs/ai-mirror/

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LLM_DOCS_SRC="${SCRIPT_DIR}/llm-docs"
LLM_DOCS_TARGET="${HOME}/.local/share/llm-docs/ai-mirror"

# 颜色
RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m'

log() { echo -e "${GREEN}[publish]${NC} $*"; }
info() { echo -e "${CYAN}[info]${NC} $*"; }
error() { echo -e "${RED}[error]${NC} $*" >&2; }

# 检查源目录
if [[ ! -d "$LLM_DOCS_SRC" ]]; then
	error "llm-docs/ 目录不存在: $LLM_DOCS_SRC"
	exit 1
fi

# 检查 README.md
if [[ ! -f "$LLM_DOCS_SRC/README.md" ]]; then
	error "README.md 缺失，必须存在"
	exit 1
fi

# 检查文件格式
for f in "$LLM_DOCS_SRC"/*.md; do
	if [[ ! -f "$f" ]]; then
		error "非 .md 文件: $f"
		exit 1
	fi
done

# 检查隐私信息
info "检查隐私信息..."
for f in "$LLM_DOCS_SRC"/*.md; do
	# 检查邮箱
	if grep -qE '[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,}' "$f" 2>/dev/null; then
		error "发现邮箱地址: $f"
		exit 1
	fi
	# 检查 git 仓库地址
	if grep -qE 'git@[a-zA-Z0-9.-]+:[a-zA-Z0-9._/-]+' "$f" 2>/dev/null; then
		error "发现 git 地址: $f"
		exit 1
	fi
	# 检查 IP 地址（排除 127.0.0.1 和 localhost）
	if grep -qE '[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}' "$f" 2>/dev/null | grep -vE '127\.0\.0\.1|localhost'; then
		warn "发现 IP 地址: $f（请确认是否需要保留）"
	fi
done

# 创建目标目录
mkdir -p "$LLM_DOCS_TARGET"

# rsync 镜像同步
log "同步到 $LLM_DOCS_TARGET..."
rsync -av --delete "$LLM_DOCS_SRC/" "$LLM_DOCS_TARGET/"

# 统计
count=$(find "$LLM_DOCS_TARGET" -name "*.md" | wc -l)
log "发布完成：$count 个文档文件"

# 列出文件
info "已发布文件："
find "$LLM_DOCS_TARGET" -name "*.md" | sort | while read -r f; do
	echo "  $f"
done
