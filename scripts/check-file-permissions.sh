#!/usr/bin/env bash
# check-file-permissions.sh — Ensure source files have 600 permissions
# Called by .pre-commit-config.yaml
# All source code files (.cpp, .hpp, .h, .py, .sh) must be 600

set -euo pipefail

failed=0

# Check C++ source files (must be 600)
while IFS= read -r -d '' file; do
	perms=$(stat -c '%a' "$file")
	if [[ "$perms" != "600" ]]; then
		echo "ERROR: $file has permissions $perms (expected 600)"
		failed=1
	fi
done < <(git diff --cached --name-only --diff-filter=ACMR -z \
	-- '*.cpp' '*.hpp' '*.h' '*.cxx' '*.cc' 2>/dev/null | while IFS= read -r -d '' f; do
	[[ -f "$f" ]] && printf '%s\0' "$f"
done)

# Check Python files (must be 600)
while IFS= read -r -d '' file; do
	perms=$(stat -c '%a' "$file")
	if [[ "$perms" != "600" ]]; then
		echo "ERROR: $file has permissions $perms (expected 600)"
		failed=1
	fi
done < <(git diff --cached --name-only --diff-filter=ACMR -z \
	-- '*.py' 2>/dev/null | while IFS= read -r -d '' f; do
	[[ -f "$f" ]] && printf '%s\0' "$f"
done)

# Check all shell scripts (must be 600)
while IFS= read -r -d '' file; do
	perms=$(stat -c '%a' "$file")
	if [[ "$perms" != "600" ]]; then
		echo "ERROR: $file has permissions $perms (expected 600)"
		failed=1
	fi
done < <(git diff --cached --name-only --diff-filter=ACMR -z \
	-- '*.sh' 2>/dev/null | while IFS= read -r -d '' f; do
	[[ -f "$f" ]] && printf '%s\0' "$f"
done)

if [[ "$failed" -eq 1 ]]; then
	echo ""
	echo "Fix: chmod 600 <file>"
	exit 1
fi

exit 0
