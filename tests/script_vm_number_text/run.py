#!/usr/bin/env python3
"""The engine's "%.7f" for floats written to script attributes (core number_text.hpp) against printf, over 14 million
floats: random bit patterns, game-like values, every exponent's edges and the exact ties."""
import os
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
core = here.parents[1] / 'src/libs/core'
with tempfile.TemporaryDirectory(prefix='storm-script-vm-number-text-') as directory:
    binary = Path(directory) / 'check'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-O2', f'-I{core}/include', f'-I{core}/src',
                    str(here / 'check.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
