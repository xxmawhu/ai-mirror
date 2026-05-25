#!/usr/bin/env bash
# ai-mirror post-merge hook: auto-install after git pull
# Managed by pre-commit framework, configured in .pre-commit-config.yaml
set -euo pipefail

main() {
	local SCRIPT_DIR
	SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
	local PROJECT_DIR
	PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

	echo "=== post-merge: deploying ai-mirror ==="
	cd "$PROJECT_DIR"

	# Re-run setup-hooks to ensure hooks are up-to-date
	if [[ -f "scripts/setup-hooks.sh" ]]; then
		bash scripts/setup-hooks.sh 2>&1 || true
	fi

	# Build and install ai-mirror (|| true to avoid blocking git pull)
	bash install.sh 2>&1 || true
	echo "=== deploy complete ==="
}

main "$@"
