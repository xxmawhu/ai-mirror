#!/usr/bin/env bash
# ai-mirror post-merge hook: auto-install after git pull
# Managed by pre-commit framework, configured in .pre-commit-config.yaml
# [log-review] 日志输出到 ./log/hook/ (Rule 2/9)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
LOG_DIR="$PROJECT_DIR/log/hook"
LOG_FILE="$LOG_DIR/post-merge-$(date +%Y-%m-%d).log"

# Ensure log directory exists
mkdir -p "$LOG_DIR"

# Tee all output to log file (Rule 2: screen output must tee to ./log/)
exec > >(tee -a "$LOG_FILE") 2>&1

main() {
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
