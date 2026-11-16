#!/usr/bin/env python3
"""CPU-only production recovery policy and exact primed output ownership controls; no inference/GPU."""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='storm-tile-policy-') as directory:
    binary = Path(directory) / 'check'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
        '-I' + str(root / 'src/libs/relief'), str(Path(__file__).with_name('check.cpp')), '-o', str(binary)],
        check=True)  # no timeout: compiles on a loaded land queue take long
    subprocess.run([str(binary)], check=True, timeout=300)  # deadlock guard, not a time budget: the check never waits
