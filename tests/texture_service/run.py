#!/usr/bin/env python3
"""Bounded real Service scheduler control; no renderer, GPU, real textures or model runtime."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
here = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='storm-texture-service-') as directory:
    output = Path(directory) / 'check'
    source = Path(os.environ.get('STORM_TEXTURE_SERVICE_SOURCE', str(root / 'src/libs/renderer_bgfx/src/texture/core/service.cpp')))
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-pthread', '-Wall', '-Wextra', '-Werror',
        '-fno-access-control', # fixture synchronizes with the real shutdown mutex; no production testing hook
        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
        '-I' + str(here / 'stubs'), '-I' + str(root / 'src/libs/renderer_bgfx/src'),
        str(here / 'check.cpp'), str(source), '-o', str(output)], check=True, timeout=35)
    subprocess.run([str(output)], check=True, timeout=15)
