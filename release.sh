#!/bin/bash
set -euo pipefail

REMOTE="${1:-gitee}"
BRANCH="main"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP_DIR="/tmp/ai-mirror-release-$(date +%s)"

info() { echo "[INFO] $1"; }
err() { echo "[ERROR] $1" >&2; }
cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT

main() {
    info "=== ai-mirror release ==="
    remote_url=$(git -C "$SCRIPT_DIR" remote get-url "$REMOTE") || { err "Remote '$REMOTE' not found"; exit 1; }
    
    info "Building clean repo..."
    mkdir -p "$TMP_DIR/repo"
    git -C "$SCRIPT_DIR" archive HEAD | tar -x -C "$TMP_DIR/repo"
    
    git -C "$TMP_DIR/repo" init -b "$BRANCH"
    git -C "$TMP_DIR/repo" add -A
    git -C "$TMP_DIR/repo" commit -m "release $(date +%Y-%m-%d)"
    
    info "Pushing to $REMOTE..."
    git -C "$TMP_DIR/repo" remote add origin "$remote_url"
    git -C "$TMP_DIR/repo" push -f origin "$BRANCH"
    
    local count
    count=$(git -C "$TMP_DIR/repo" ls-files | wc -l)
    info "Done. $count files pushed."
}

main
