#!/usr/bin/env python3
"""Bounded CPU-only production upload policy and actual LoadingOpaque receipt controls."""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
here = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='storm-loading-upload-') as directory:
    output = Path(directory) / 'check'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
        '-I' + str(root / 'src/libs/renderer_bgfx/src'), '-I' + str(root / 'src/libs/renderer_bgfx/include'),
        str(here / 'check.cpp'), '-o', str(output)], check=True)  # no timeout: compiles on a loaded land queue take long
    subprocess.run([str(output)], check=True, timeout=300)  # deadlock guard, not a time budget: the check never waits
