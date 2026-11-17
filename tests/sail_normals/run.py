#!/usr/bin/env python3
"""Exercise the actual FillVertex function without linking the game or a renderer, and the per-frame sail normals
(src/libs/rigging/src/sail_normals.hpp, grid_check.cpp)."""
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
source = (root / 'src/libs/rigging/src/sailone.cpp').read_text()
start = source.index('void SAILONE::FillVertex(')
end = source.index('\nvoid SAILONE::SetTexGrid(', start)
actual = source[start:end]
# Mutation control: reproduce the former square-only normal-origin error; other paths must stay identical.
old = actual.replace('SAILONE::FillVertex(', 'SAILONE::OldFillVertex(')
square = old.index('dnorm = ss.boundSphere.c', old.index('    else\n'))
old = old[:square] + old[square:].replace('dnorm = ss.boundSphere.c - sgeo.cv.normL * ss.boundSphere.r;',
                                       'dnorm = ss.boundSphere.c - (ss.eSailType == SAIL_SQUARE ? sgeo.cv.dnormL : sgeo.cv.normL) * ss.boundSphere.r;', 1)
with tempfile.TemporaryDirectory(prefix='storm-sail-normals-') as directory:
    directory = Path(directory)
    unit = directory / 'check.cpp'
    unit.write_text(Path(__file__).with_name('check.cpp').read_text().replace('// INSERT_ACTUAL_FILL_VERTEX', actual + old))
    binary = directory / 'check'
    subprocess.run([*shlex.split(os.environ.get('CXX', 'c++')), '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror',
                    str(unit), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    grid = directory / 'grid_check'
    subprocess.run([*shlex.split(os.environ.get('CXX', 'c++')), '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', *math_include_flags(args.glm_source),
                    str(Path(__file__).with_name('grid_check.cpp')), '-o', str(grid)], check=True)
    subprocess.run([str(grid)], check=True)
