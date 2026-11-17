#!/usr/bin/env python3
"""The land menu refresh gate re-reads the scripts only on the original game's invalidation, never on a timer."""
import os
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]

with tempfile.TemporaryDirectory(prefix='storm-hud-invalidate-') as directory:
    binary = Path(directory) / 'check'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-fsanitize=address,undefined',
                    '-I' + str(root / 'src/libs/util/include'), str(here / 'check.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
