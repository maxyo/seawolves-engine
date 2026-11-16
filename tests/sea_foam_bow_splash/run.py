#!/usr/bin/env python3
"""The storm's bow splash gate (src/libs/sea_foam/src/bow_splash.hpp): every ship splashes from its own bow's rise."""
import pathlib, subprocess, tempfile, sys
here = pathlib.Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='sea-foam-bow-splash-') as tmp:
    binary = str(pathlib.Path(tmp) / 'check')
    subprocess.run(['c++', '-std=c++17', '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(here / 'check.cpp'), '-o', binary], check=True)
    sys.exit(subprocess.run([binary]).returncode)
