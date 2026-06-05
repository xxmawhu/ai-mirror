#!/usr/bin/env bash
#
# docker-test/run-tests.sh — One-click Podman build + test for ai-mirror
# Usage: bash docker-test/run-tests.sh [--no-cache] [--keep]
#   --no-cache   Force rebuild Podman image from scratch
#   --keep       Keep container after test (default: auto-remove)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

IMAGE_TAG="ai-mirror-test"
NO_CACHE_FLAG=""
RM_FLAG="--rm"

parse_args() {
	for arg in "$@"; do
		case "$arg" in
		--no-cache) NO_CACHE_FLAG="--no-cache" ;;
		--keep) RM_FLAG="" ;;
		-h | --help)
			echo "Usage: $0 [--no-cache] [--keep]"
			echo "  --no-cache   Force rebuild Podman image"
			echo "  --keep       Keep container after test"
			exit 0
			;;
		*)
			echo "Unknown option: $arg" >&2
			exit 1
			;;
		esac
	done
}

main() {
	parse_args "$@"

	echo "=== ai-mirror Podman Test Runner ==="
	echo ""

	# Step 1: Build Podman image
	echo "--- Building Podman image ---"
	# shellcheck disable=SC2086
	podman build ${NO_CACHE_FLAG} -t "$IMAGE_TAG" \
		-f "$PROJECT_DIR/tests/Dockerfile.test" "$PROJECT_DIR"

	echo ""
	echo "--- Running tests in privileged container ---"

	# Step 2: Run tests in privileged container
	# --privileged needed for: mount, useradd, sshd, bind mounts
	# shellcheck disable=SC2086
	podman run ${RM_FLAG} --privileged "$IMAGE_TAG"

	echo ""
	echo "=== ALL TESTS PASSED ==="
}

main "$@"
