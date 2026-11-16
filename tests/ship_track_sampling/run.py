#!/usr/bin/env python3
"""The ship trail's sampling (src/libs/ship/src/track_sampling.hpp): distance-based emission, widths, lift and foam breakup."""
import pathlib, subprocess, tempfile, sys
here = pathlib.Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='ship-track-sampling-') as tmp:
    binary = str(pathlib.Path(tmp) / 'check')
    subprocess.run(['c++', '-std=c++17', '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(here / 'check.cpp'), '-o', binary], check=True)
    sys.exit(subprocess.run([binary]).returncode)
