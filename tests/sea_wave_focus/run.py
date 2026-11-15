#!/usr/bin/env python3
"""Bounded CPU coverage controls; --legacy proves the old 45 m cap regression."""
import pathlib, subprocess, tempfile, sys
here = pathlib.Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='sea-wave-focus-') as tmp:
    binary = str(pathlib.Path(tmp) / 'check')
    command = ['c++', '-std=c++17', '-O1', '-g', '-fsanitize=address,undefined',
               '-fno-omit-frame-pointer', str(here / 'check.cpp'), '-o', binary]
    if '--legacy' in sys.argv: command.append('-DLEGACY_FOCUS')
    subprocess.run(command, check=True)
    sys.exit(subprocess.run([binary]).returncode)
