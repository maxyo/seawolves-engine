#!/usr/bin/env python3
"""Check the player's camera fire side selection (fire_side.hpp)."""
import os
from pathlib import Path
import subprocess
import tempfile

r = Path(__file__).resolve().parents[2]

with tempfile.TemporaryDirectory(prefix='sea-fire-side-') as tmp:
    binary = Path(tmp)/'check'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Werror', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-I'+str(r/'src/libs/util/include'),
                    str(Path(__file__).with_name('check.cpp')), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
