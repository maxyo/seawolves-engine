#!/usr/bin/env python3
"""The HUD's time acceleration steps against the stock scripts' counter ladder, limits and interface exit
(stock and as the app's script patch interface-exit-time-reset changes it)."""
import os
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='storm-time-scale-') as d:
    binary = Path(d)/'check'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Werror', '-fsanitize=address,undefined',
                    '-I'+str(root/'src/apps/engine/src'), str(Path(__file__).with_name('check.cpp')),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
