#!/usr/bin/env bash
#
# ai-mirror hooks setup script
# Installs pre-commit framework, configures hooks, and sets up commit/merge hooks.
#
# Usage:
#   bash scripts/setup-hooks.sh          # full setup
#   bash scripts/setup-hooks.sh --check  # check only, no install
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

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
	((ERRORS++))
}
log_warn() { echo -e "${YELLOW}  ⚠️  $1${NC}"; }
log_info() { echo -e "${CYAN}  ℹ️  $1${NC}"; }

echo -e "${CYAN}=== ai-mirror hooks setup ===${NC}"

# ============================================
# Step 1: Check / install pre-commit
# ============================================
echo ""
echo -e "${CYAN}--- Step 1: pre-commit ---${NC}"

if command -v pre-commit &>/dev/null; then
	PC_VERSION=$(pre-commit --version 2>&1)
	log_ok "pre-commit installed: $PC_VERSION"
else
	log_warn "pre-commit not found, installing via pip..."
	if command -v pip &>/dev/null; then
		pip install pre-commit 2>&1
		log_ok "pre-commit installed via pip"
	elif command -v pip3 &>/dev/null; then
		pip3 install pre-commit 2>&1
		log_ok "pre-commit installed via pip3"
	else
		log_err "cannot install pre-commit: pip/pip3 not found"
	fi
fi

# ============================================
# Step 2: Check .pre-commit-config.yaml
# ============================================
echo ""
echo -e "${CYAN}--- Step 2: pre-commit config ---${NC}"

PC_CONFIG="$PROJECT_DIR/.pre-commit-config.yaml"
if [ -f "$PC_CONFIG" ]; then
	log_ok ".pre-commit-config.yaml exists"
else
	log_err ".pre-commit-config.yaml not found (should be tracked in git)"
fi

# ============================================
# Step 3: Install pre-commit hooks
# ============================================
echo ""
echo -e "${CYAN}--- Step 3: pre-commit install ---${NC}"

CHECK_MODE="${1:-}"
if [ "$CHECK_MODE" = "--check" ]; then
	if [ -f "$PROJECT_DIR/.git/hooks/pre-commit" ]; then
		log_ok ".git/hooks/pre-commit exists"
	else
		log_err ".git/hooks/pre-commit not found"
	fi
else
	if command -v pre-commit &>/dev/null && [ -f "$PC_CONFIG" ]; then
		cd "$PROJECT_DIR"
		pre-commit install 2>&1 || true
		log_ok "pre-commit hooks installed"
	else
		log_warn "skipping pre-commit install (missing pre-commit or config)"
	fi
fi

# ============================================
# Step 4: Set up commit-hook (three-phase)
# ============================================
echo ""
echo -e "${CYAN}--- Step 4: commit-hook (three-phase) ---${NC}"

COMMIT_HOOK="$PROJECT_DIR/scripts/commit-hook.sh"
PRE_COMMIT_LINK="$PROJECT_DIR/.git/hooks/pre-commit"

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
# Step 5: Set up post-merge hook
# ============================================
echo ""
echo -e "${CYAN}--- Step 5: post-merge hook ---${NC}"

POST_MERGE="$PROJECT_DIR/.git/hooks/post-merge"

if [ "$CHECK_MODE" = "--check" ]; then
	if [ -f "$POST_MERGE" ]; then
		log_ok ".git/hooks/post-merge exists"
	else
		log_err ".git/hooks/post-merge not found"
	fi
else
	# Always create/update post-merge hook
	cat >"$POST_MERGE" <<'POSTMERGE'
#!/usr/bin/env bash
# ai-mirror post-merge hook: deploy after merge/pull
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"

echo "=== post-merge: deploying ai-mirror ==="
cd "$PROJECT_DIR"

# Self-update hooks from latest code
if [ -f "scripts/setup-hooks.sh" ]; then
	bash scripts/setup-hooks.sh 2>&1 || true
fi

bash install.sh 2>&1
echo "=== deploy complete ==="
POSTMERGE
	chmod +x "$POST_MERGE"
	log_ok "updated .git/hooks/post-merge"
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
