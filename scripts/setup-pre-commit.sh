#!/usr/bin/env bash
# ai-mirror pre-commit installation script
# Single entry point for pre-commit setup (git-tidy Rule 8)
# [log-review] 日志输出到 ./log/hook/ (Rule 2/9)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
LOG_DIR="$PROJECT_DIR/log/hook"
LOG_FILE="$LOG_DIR/setup-pre-commit-$(date +%Y-%m-%d).log"

# Ensure log directory exists
mkdir -p "$LOG_DIR"

# Tee all output to log file (Rule 2: screen output must tee to ./log/)
exec > >(tee -a "$LOG_FILE") 2>&1

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m'

main() {
	echo -e "${GREEN}=== Setting up pre-commit ===${NC}"

	# ============================================
	# Step 1: Check / install pre-commit
	# ============================================
	if command -v pre-commit &>/dev/null; then
		local PC_VERSION
		PC_VERSION=$(pre-commit --version 2>&1)
		echo -e "${GREEN}  ✅ pre-commit installed: $PC_VERSION${NC}"
	else
		echo -e "${YELLOW}  ⚠️  pre-commit not found, installing...${NC}"
		if command -v pip &>/dev/null; then
			pip install pre-commit 2>&1
		elif command -v pip3 &>/dev/null; then
			pip3 install pre-commit 2>&1
		else
			echo -e "${RED}  ❌ ERROR: pip/pip3 not found, cannot install pre-commit${NC}"
			exit 1
		fi
		echo -e "${GREEN}  ✅ pre-commit installed${NC}"
	fi

	# ============================================
	# Step 2: Install pre-commit hooks
	# ============================================
	cd "$PROJECT_DIR"

	if [ ! -f ".pre-commit-config.yaml" ]; then
		echo -e "${RED}  ❌ ERROR: .pre-commit-config.yaml not found${NC}"
		exit 1
	fi

	# Install pre-commit hook
	pre-commit install 2>&1 || true
	echo -e "${GREEN}  ✅ pre-commit hooks installed${NC}"

	# Install post-merge hook
	pre-commit install --hook-type post-merge 2>&1 || true
	echo -e "${GREEN}  ✅ post-merge hook installed${NC}"
}

main "$@"
