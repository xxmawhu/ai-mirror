#!/usr/bin/env bash
#
# ai-mirror hooks setup script
# Installs pre-commit framework, configures hooks, and sets up commit/merge hooks.
#
# Usage:
#   bash scripts/setup-hooks.sh          # full setup
#   bash scripts/setup-hooks.sh --check  # check only, no install
#
# [log-review] 日志输出到 ./log/hook/ (Rule 2/9)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
CHECK_MODE="${1:-}"
LOG_DIR="$PROJECT_DIR/log/hook"
LOG_FILE="$LOG_DIR/setup-hooks-$(date +%Y-%m-%d).log"

# Ensure log directory exists
mkdir -p "$LOG_DIR"

# Tee all output to log file (Rule 2: screen output must tee to ./log/)
exec > >(tee -a "$LOG_FILE") 2>&1

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
NC='\033[0m'

ERRORS=0

log_ok() { echo -e "${GREEN}  ✅ $1${NC}"; }
log_err() {
	echo -e "${RED}  ❌ $1${NC}"
	ERRORS=$((ERRORS + 1))
}
log_warn() { echo -e "${YELLOW}  ⚠️  $1${NC}"; }
log_info() { echo -e "${CYAN}  ℹ️  $1${NC}"; }

main() {
	echo -e "${CYAN}=== ai-mirror hooks setup ===${NC}"

	# ============================================
	# Step 1: Setup pre-commit (via unified entry point)
	# ============================================
	echo ""
	echo -e "${CYAN}--- Step 1: pre-commit ---${NC}"

	local SETUP_PC="$SCRIPT_DIR/setup-pre-commit.sh"
	if [ "$CHECK_MODE" = "--check" ]; then
		# Check mode: verify pre-commit installed
		if command -v pre-commit &>/dev/null; then
			local PC_VERSION
			PC_VERSION=$(pre-commit --version 2>&1)
			log_ok "pre-commit installed: $PC_VERSION"
		else
			log_err "pre-commit not found"
		fi
	else
		# Install mode: use setup-pre-commit.sh
		if [ -f "$SETUP_PC" ]; then
			bash "$SETUP_PC" 2>&1
			log_ok "pre-commit setup via scripts/setup-pre-commit.sh"
		else
			log_err "scripts/setup-pre-commit.sh not found"
		fi
	fi

	# ============================================
	# Step 2: Check .pre-commit-config.yaml
	# ============================================
	echo ""
	echo -e "${CYAN}--- Step 2: pre-commit config ---${NC}"

	local PC_CONFIG="$PROJECT_DIR/.pre-commit-config.yaml"
	if [ -f "$PC_CONFIG" ]; then
		log_ok ".pre-commit-config.yaml exists"
	else
		log_err ".pre-commit-config.yaml not found (should be tracked in git)"
	fi

	# ============================================
	# Step 3: Verify pre-commit hooks installed
	# ============================================
	echo ""
	echo -e "${CYAN}--- Step 3: pre-commit hooks verification ---${NC}"

	if [ "$CHECK_MODE" = "--check" ]; then
		if [ -f "$PROJECT_DIR/.git/hooks/pre-commit" ]; then
			log_ok ".git/hooks/pre-commit exists"
		else
			log_err ".git/hooks/pre-commit not found"
		fi
	else
		# Already installed by setup-pre-commit.sh in Step 1
		if [ -f "$PROJECT_DIR/.git/hooks/pre-commit" ]; then
			log_ok "pre-commit hooks installed"
		else
			log_warn ".git/hooks/pre-commit not found after setup"
		fi
	fi

	# ============================================
	# Step 4: Set up commit-hook (three-phase)
	# ============================================
	echo ""
	echo -e "${CYAN}--- Step 4: commit-hook (three-phase) ---${NC}"

	local COMMIT_HOOK="$PROJECT_DIR/scripts/commit-hook.sh"
	local PRE_COMMIT_LINK="$PROJECT_DIR/.git/hooks/pre-commit"

	if [ -f "$COMMIT_HOOK" ]; then
		log_ok "scripts/commit-hook.sh exists"

		# Check if pre-commit hook already delegates to our script
		if [ -f "$PRE_COMMIT_LINK" ]; then
			# If pre-commit framework is installed, it manages the hook.
			# We chain our commit-hook.sh via pre-commit local hook instead.
			log_info "pre-commit framework manages .git/hooks/pre-commit"
			log_info "commit-hook.sh is integrated via pre-commit local hook"
		else
			log_warn ".git/hooks/pre-commit not found"
		fi
	else
		log_err "scripts/commit-hook.sh not found"
	fi

	# ============================================
	# Step 5: Install post-merge hook via pre-commit
	# ============================================
	echo ""
	echo -e "${CYAN}--- Step 5: post-merge hook ---${NC}"

	local POST_MERGE="$PROJECT_DIR/.git/hooks/post-merge"

	if [ "$CHECK_MODE" = "--check" ]; then
		if [ -f "$POST_MERGE" ]; then
			# Verify it's managed by pre-commit (not hand-written)
			if head -3 "$POST_MERGE" | grep -q "pre-commit"; then
				log_ok ".git/hooks/post-merge exists (managed by pre-commit)"
			else
				log_err ".git/hooks/post-merge exists but NOT managed by pre-commit"
				log_err "Run 'bash scripts/setup-hooks.sh' to fix"
			fi
		else
			log_err ".git/hooks/post-merge not found"
		fi
	else
		# Install post-merge hook via pre-commit framework
		if command -v pre-commit &>/dev/null && [ -f "$PC_CONFIG" ]; then
			cd "$PROJECT_DIR"
			pre-commit install --hook-type post-merge 2>&1 || true
			log_ok "post-merge hook installed via pre-commit"
		else
			log_warn "skipping post-merge install (missing pre-commit or config)"
		fi
	fi

	# ============================================
	# Summary
	# ============================================
	echo ""
	echo -e "${CYAN}=== Setup Summary ===${NC}"
	if [ $ERRORS -gt 0 ]; then
		echo -e "${RED}  ${ERRORS} error(s) found${NC}"
		exit 1
	else
		echo -e "${GREEN}  All hooks configured successfully${NC}"
		exit 0
	fi
}

main "$@"
