#!/usr/bin/env python3
"""HUD state channels: JSON built only on a typed change, publications identical to the text compare.

`run.py --bench` also builds an optimized binary and prints the cost of one unchanged loop per
channel, old publisher against new.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]

with tempfile.TemporaryDirectory(prefix='storm-hud-channels-') as directory:
    compiler = os.environ.get('CXX', 'c++')
    include = '-I' + str(root / 'src/libs/util/include')
    binary = Path(directory) / 'check'
    subprocess.run([compiler, '-std=c++20', '-fsanitize=address,undefined', include, str(here / 'check.cpp'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    if '--bench' in sys.argv[1:]:
        bench = Path(directory) / 'bench'
        subprocess.run([compiler, '-std=c++20', '-O2', '-DNDEBUG', include, str(here / 'check.cpp'), '-o', str(bench)],
                       check=True)
        subprocess.run([str(bench), '--bench'], check=True)
