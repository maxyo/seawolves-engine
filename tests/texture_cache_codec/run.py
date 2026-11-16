#!/usr/bin/env python3
"""The upscale and relief cache files (renderer_bgfx/fx: upscale.cpp, relief_store.cpp): round trip, end of file,
truncation, the read reservation and a failed rename (codec.cpp). No renderer, GPU or models."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
fx = root / 'src/libs/renderer_bgfx/fx'
here = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='storm-cache-codec-') as directory:
    work = Path(directory)
    binary = work / 'check'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-O0', '-ffunction-sections', '-fdata-sections',
                    '-Wl,--gc-sections', '-I' + str(fx / 'include'), str(here / 'codec.cpp'), str(fx / 'src/upscale.cpp'),
                    str(fx / 'src/relief_store.cpp'), str(fx / 'src/normal_map.cpp'), '-o', str(binary)], check=True)
    sys.exit(subprocess.run([str(binary), str(work)]).returncode)
