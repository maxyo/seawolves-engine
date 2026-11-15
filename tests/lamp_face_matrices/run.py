#!/usr/bin/env python3
"""Exact production lamp face/submit fragments with the pinned bx math; no renderer/device link.

Builds the check with CMake (CMakeLists.txt here: fuse.py writes the unit, the pinned bx comes from the shared
FetchContent cache, bx_pin.cmake) in the kept --build-dir, runs it, then makes sure each mutation is rejected.
Offline: set FETCHCONTENT_SOURCE_DIR_STORM_BX in the environment.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build-dir', type=Path, required=True)
a = p.parse_args()
here = Path(__file__).resolve().parent
build = a.build_dir.resolve()
configure = ['cmake', '-S', str(here), '-B', str(build), '-G', 'Ninja', f'-DPython3_EXECUTABLE={sys.executable}']
if os.environ.get('FETCHCONTENT_SOURCE_DIR_STORM_BX'):
    configure.append(f'-DFETCHCONTENT_SOURCE_DIR_STORM_BX={os.environ["FETCHCONTENT_SOURCE_DIR_STORM_BX"]}')
subprocess.run(configure, check=True, stdout=subprocess.DEVNULL)
warmth = here.parents[1] / 'tools/ccache/warmth'
subprocess.run([str(warmth), 'begin', str(build)], check=True)
subprocess.run(['cmake', '--build', str(build)], check=True)
subprocess.run([str(warmth), 'end', str(build)], check=True)
binary = str(build / 'lamp_face_matrices_check')
subprocess.run([binary], check=True)
for mutation in ('no-reset', 'uncached'):
    result = subprocess.run([binary, mutation], capture_output=True, text=True)
    if result.returncode != 1 or 'FAIL:' not in result.stderr:
        raise RuntimeError('Mutation was not rejected: ' + mutation + result.stderr)
    print('PASS rejected ' + mutation + ': ' + result.stderr.strip())
