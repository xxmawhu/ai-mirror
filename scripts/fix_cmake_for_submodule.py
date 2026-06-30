#!/usr/bin/env python3
"""Remove test blocks from CMakeLists.txt with proper if/endif nesting handling."""
import re, sys

if len(sys.argv) != 2:
    print(f"Usage: {sys.argv[0]} <CMakeLists.txt>", file=sys.stderr)
    sys.exit(1)

path = sys.argv[1]
with open(path, "r") as f:
    lines = f.readlines()

result = []
skip_depth = 0
in_test = False

for line in lines:
    s = line.strip()

    # Detect start of test block
    if not in_test and re.match(r'^\s*if\s*\(EXISTS.*tests\b', s, re.I):
        in_test = True
        skip_depth = 1
        continue

    if in_test:
        if re.match(r'^\s*if\b', s, re.I):
            skip_depth += 1
        elif re.match(r'^\s*endif\b', s, re.I):
            skip_depth -= 1
            if skip_depth == 0:
                in_test = False
            continue
        continue

    # Remove standalone enable_testing/add_test lines
    if re.match(r'^\s*(enable_testing|add_test)\s*\(', s, re.I):
        continue

    result.append(line)

with open(path, "w") as f:
    f.writelines(result)

print(f"Fixed {path}: removed test blocks ({len(lines)} -> {len(result)} lines)")
