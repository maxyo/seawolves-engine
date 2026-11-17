#!/usr/bin/env python3
"""Host check of the background file reads of loads started while the game is played (src/libs/core/src/background_read.cpp:
the reader thread, candidates in order, the bytes a PrefetchScope hands to the readers) and of the texture names read
from a model file before it is built (src/libs/geometry/src/geom_texture_names.hpp). No game data."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--dependency-sources', type=Path, required=True)
p.add_argument('--glm-source', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
deps = a.dependency_sources.resolve()
with tempfile.TemporaryDirectory(prefix='storm-background-read-') as directory:
    binary = Path(directory) / 'check'
    includes = [root / 'src/libs/core/include', root / 'src/libs/geometry/src', root / 'src/libs/math/include',
                deps / 'fmt/include', deps / 'spdlog/include', a.glm_source.resolve()]
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Wno-multichar',
                    '-DFMT_HEADER_ONLY', '-DSPDLOG_FMT_EXTERNAL', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-pthread', *['-I' + str(i) for i in includes],
                    str(Path(__file__).with_name('check.cpp')), str(root / 'src/libs/core/src/background_read.cpp'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary), directory], check=True)
