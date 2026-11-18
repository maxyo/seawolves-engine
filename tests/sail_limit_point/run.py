#!/usr/bin/env python3
"""The bulge of a sail with a limit point is the original engine's value (src/libs/rigging/src/sail_limit_point.hpp)."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / 'tests/glm_math'))
from glm_source import math_include_flags  # noqa: E402
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--glm-source', type=Path)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='storm-sail-limit-point-') as directory:
    binary = Path(directory) / 'check'
    subprocess.run([*shlex.split(os.environ.get('CXX', 'c++')), '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', *math_include_flags(args.glm_source),
                    str(Path(__file__).with_name('check.cpp')), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
