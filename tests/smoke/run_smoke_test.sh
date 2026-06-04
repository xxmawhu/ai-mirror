#!/usr/bin/env bash
#
# ai-mirror smoke test runner
# Builds a Docker image and runs smoke tests inside the container.
#
# Usage:
#   bash tests/smoke/run_smoke_test.sh           # run smoke tests
#   bash tests/smoke/run_smoke_test.sh --no-rm    # keep container after test
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
IMAGE_NAME="ai-mirror-smoke"
NO_RM="${1:-}"

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m'

echo -e "${CYAN}=== ai-mirror Smoke Test ===${NC}"
echo "  Project: $PROJECT_DIR"
echo "  Image: $IMAGE_NAME"
echo ""

# Build Docker image
echo -e "${CYAN}[1/2] Building Docker image...${NC}"
docker build -t "$IMAGE_NAME" -f "$PROJECT_DIR/docker/Dockerfile.smoke" "$PROJECT_DIR"

if [ $? -ne 0 ]; then
	echo -e "${RED}Docker build failed${NC}"
	exit 1
fi
echo -e "${GREEN}Docker image built${NC}"
echo ""

# Run smoke test
echo -e "${CYAN}[2/2] Running smoke tests...${NC}"
RM_FLAG="--rm"
if [ "$NO_RM" = "--no-rm" ]; then
	RM_FLAG=""
fi

docker run $RM_FLAG --privileged "$IMAGE_NAME"
EXIT_CODE=$?

echo ""
if [ $EXIT_CODE -eq 0 ]; then
	echo -e "${GREEN}=== SMOKE TEST PASSED ===${NC}"
else
	echo -e "${RED}=== SMOKE TEST FAILED (exit code: $EXIT_CODE) ===${NC}"
fi

exit $EXIT_CODE
