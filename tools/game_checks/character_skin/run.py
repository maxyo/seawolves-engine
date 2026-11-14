#!/usr/bin/env python3
"""Game-data check: the characters' skin on their torso bones (src/libs/renderer_bgfx/fx/include/storm/fx/skin_tone.hpp,
src/libs/animation/include/skin_bones.hpp) over every character model of an installed game: per model atlas the skin
reference from its face and hands and the share of its torso and arm texels the colour test counts as skin; models far
above the rest are flagged for a look (a bare chest, or clothes in a skin colour).

  run.py <game or RESOURCE directory> [--list N] [--model NAME]..."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

if len(sys.argv) < 2:
    raise SystemExit(__doc__)
here = Path(__file__).resolve().parent
root = here.parents[2]
glm = os.environ.get('STORM_GLM_INCLUDE')
if not glm:
    cached = sorted((p for p in Path.home().glob('.cache/storm-deps/fetchcontent/storm_glm-*') if p.is_dir()),
                    key=lambda p: p.stat().st_mtime)
    if not cached:
        raise SystemExit('no GLM: set STORM_GLM_INCLUDE or configure an engine build first (cmake/glm.cmake)')
    glm = str(cached[-1])
with tempfile.TemporaryDirectory(prefix='storm-character-skin-') as directory:
    binary = Path(directory) / 'check'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'src/libs/animation/include'), '-I' + str(root / 'src/libs/math/include'),
                    '-I' + str(root / 'src/libs/renderer_bgfx/fx/include'), '-I' + glm,
                    str(here / 'check.cpp'), str(root / 'src/libs/renderer_bgfx/fx/src/skin_tone.cpp'), '-o', str(binary)],
                   check=True)
    subprocess.run([str(binary), *sys.argv[1:]], check=True)
