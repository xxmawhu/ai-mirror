#!/usr/bin/env bash
set -euo pipefail

AM_BIN="${AM_BIN:-/usr/local/bin/ai-mirror-bin}"

if [ ! -x "$AM_BIN" ]; then
	echo "error: $AM_BIN not found" >&2
	exit 1
fi

if [ "$(id -u)" -eq 0 ]; then
	exec "$AM_BIN" "$@"
fi

exec sudo "$AM_BIN" "$@"
