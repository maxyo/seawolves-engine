#!/usr/bin/env python3
"""Game-data check: the skin bones (src/libs/animation/include/skin_bones.hpp: the head with the neck, and the hands,
where the characters' relief fades out) of every skeleton (.an) of an installed game. The human skeletons of TEHO must
all have them; the animals' list "none".

  run.py <game or RESOURCE directory>"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

# TEHO's human skeletons (the T-pose ones the characters use).
HUMAN = ['man.an', 'man1.an', 'man2.an', 'man_A1.an', 'man_B.an', 'man_C.an', 'man_KVL.an', 'man_kvl2.an',
         'woman.an', 'woman2.an', 'woman_A2.an', 'woman_B.an', 'towngirl3.an', 'mushketer.an', 'mushketer_A1.an',
         'skeleton.an', 'skeleton_B.an', 'genres.an', 'Canib.an', 'Canib_mush.an', 'Danielle.an', 'Danny.an',
         'Angellica.an', 'Mary.an', 'Rumba.an', 'YokoDias.an', 'blaze.an', 'Baker.an', 'Giant.an', 'Balthrop.an',
         'Jess.an', 'Jessika.an', 'Moscovit.an']

if len(sys.argv) < 2:
    raise SystemExit(__doc__)
here = Path(__file__).resolve().parent
root = here.parents[2]
# GLM (header-only) as the engine fetches it: $STORM_GLM_INCLUDE, else the shared dependency cache of the builds.
glm = os.environ.get('STORM_GLM_INCLUDE')
if not glm:
    cached = sorted((p for p in Path.home().glob('.cache/storm-deps/fetchcontent/storm_glm-*') if p.is_dir()), key=lambda p: p.stat().st_mtime)
    if not cached:
        raise SystemExit('no GLM: set STORM_GLM_INCLUDE or configure an engine build first (cmake/glm.cmake)')
    glm = str(cached[-1])
with tempfile.TemporaryDirectory(prefix='storm-skin-bones-') as directory:
    binary = Path(directory) / 'check'
    flags = ['-I' + str(root / 'src/libs/animation/include'), '-I' + str(root / 'src/libs/math/include')]
    flags.append('-I' + glm)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', *flags,
                    str(here / 'check.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary), sys.argv[1], *HUMAN], check=True)
