#!/bin/bash
set -euo pipefail

REMOTE="${1:-gitee}"
BRANCH="main"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP_DIR="/tmp/ai-mirror-release-$(date +%s)"

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }
cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT

main() {
    log_info "=== ai-mirror release ==="
    remote_url=$(git -C "$SCRIPT_DIR" remote get-url "$REMOTE") || exit 1
    
    log_info "Building clean repo..."
    mkdir -p "$TMP_DIR/repo"
    git -C "$SCRIPT_DIR" archive HEAD | tar -x -C "$TMP_DIR/repo"
    
    git -C "$TMP_DIR/repo" init -b "$BRANCH"
    git -C "$TMP_DIR/repo" add -A
    git -C "$TMP_DIR/repo" commit -m "release $(date +%Y-%m-%d)"
    
    log_info "Pushing to $REMOTE..."
    git -C "$TMP_DIR/repo" remote add origin "$remote_url"
    git -C "$TMP_DIR/repo" push -f origin "$BRANCH"
    
    log_info "Done. Files pushed:"
    git -C "$TMP_DIR/repo" ls-files | head -10
    log_info "Total: $(git -C "$TMP_DIR/repo" ls-files | wc -l) files"
}

main
