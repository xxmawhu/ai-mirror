#!/bin/bash
set -euo pipefail

REMOTE="${1:-gitee}"
BRANCH="main"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP_DIR="/tmp/ai-mirror-release-$(date +%s)"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

cleanup() {
    log_info "Cleaning up..."
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

main() {
    log_info "=== ai-mirror release ==="
    log_info "Remote: $REMOTE"

    remote_url=$(git -C "$SCRIPT_DIR" remote get-url "$REMOTE" 2>/dev/null) || {
        log_error "Remote '$REMOTE' not found"
        exit 1
    }

    # Step 1: Build clean repo from tracked files
    log_info "Step 1: Building clean repo..."
    mkdir -p "$TMP_DIR/repo"
    git -C "$SCRIPT_DIR" archive HEAD | tar -x -C "$TMP_DIR/repo"

    # Step 2: Init fresh repo
    git -C "$TMP_DIR/repo" init -b "$BRANCH"
    git -C "$TMP_DIR/repo" add -A
    git -C "$TMP_DIR/repo" commit -m "release: $(date +%Y-%m-%d)"

    # Step 3: Push (force to overwrite)
    log_info "Step 2: Pushing to $REMOTE..."
    git -C "$TMP_DIR/repo" remote add origin "$remote_url"
    git -C "$TMP_DIR/repo" push -f origin "$BRANCH"

    # Step 4: Clone and build test
    log_info "Step 3: Clone + build test..."
    local build_dir="$TMP_DIR/build-test"
    git clone "$remote_url" "$build_dir" 2>&1
    mkdir -p "$build_dir/build"
    cmake -S "$build_dir" -B "$build_dir/build" -DCMAKE_BUILD_TYPE=Release 2>&1 | tee "$TMP_DIR/cmake.log"
    cmake --build "$build_dir/build" 2>&1 | tee "$TMP_DIR/build.log"

    log_info "=== Release completed! ==="
}

main
