#!/usr/bin/env python3
"""The production texture Service scheduler (texture/core/service.cpp) under ASan/UBSan: cache IO isolation, fairness,
bounds, generation promotion, the pick/promotion race and shutdown (check.cpp, README.md). No renderer, GPU, real
textures or model runtime. The check waits on progress only; DEADLOCK_GUARD is not a time budget but the point at which
a step that cannot finish is reported (with the last step it printed)."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

DEADLOCK_GUARD = 300  # seconds; an idle run takes about 1 s, a land-queue run under load 80 about 4 s

root = Path(__file__).resolve().parents[2]
here = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='storm-texture-service-') as directory:
    output = Path(directory) / 'check'
    # STORM_TEXTURE_SERVICE_SOURCE: another service.cpp, e.g. an old revision, to see the check catch its defect.
    source = Path(os.environ.get('STORM_TEXTURE_SERVICE_SOURCE', root / 'src/libs/renderer_bgfx/src/texture/core/service.cpp'))
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-pthread', '-Wall', '-Wextra', '-Werror',
                    '-fno-access-control',  # fixture synchronizes with the real shutdown mutex; no production testing hook
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I' + str(here / 'stubs'), '-I' + str(root / 'src/libs/renderer_bgfx/src'),
                    str(here / 'check.cpp'), str(source), '-o', str(output)], check=True)
    try:
        result = subprocess.run([str(output)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                timeout=DEADLOCK_GUARD)
    except subprocess.TimeoutExpired as hung:
        steps = (hung.output or b'').decode(errors='replace').strip().splitlines()
        sys.exit(f'FAIL: no progress for {DEADLOCK_GUARD} s (a scheduler deadlock) at {steps[-1] if steps else "start"}')
    print(result.stdout, end='')
    sys.exit(result.returncode)
