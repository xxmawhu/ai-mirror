#!/usr/bin/env python3
import subprocess
import sys
import os
import time

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build'))

with open('build_stdout.log', 'w') as stdout_f, open('build_stderr.log', 'w') as stderr_f:
    result = subprocess.run(
        ['make', f'-j{os.cpu_count()}'],
        stdout=stdout_f,
        stderr=stderr_f,
    )

with open('build_exitcode.txt', 'w') as f:
    f.write(str(result.returncode))

sys.exit(result.returncode)
