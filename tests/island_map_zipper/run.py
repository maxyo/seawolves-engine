#!/usr/bin/env python3
"""The zipped island depth map, MapZipper (src/libs/island/src/map_zipper.cpp): loading again into a zipper that holds a
map (a missing shipped .zap, then the cached one: the island reload double free), DoZip -> Save -> Load, refused short or
garbled files. ASan/UBSan, no game data."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--dependency-sources', type=Path, required=True)
a = p.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[1]
deps = a.dependency_sources.resolve()
with tempfile.TemporaryDirectory(prefix='storm-map-zipper-') as directory:
    binary = Path(directory) / 'check'
    includes = [here / 'stubs', root / 'src/libs/island/src', root / 'src/libs/util/include',
                deps / 'fmt/include', deps / 'spdlog/include']
    subprocess.run([*os.environ.get('CXX', 'c++').split(), '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-DFMT_HEADER_ONLY', '-DSPDLOG_FMT_EXTERNAL', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-pthread', *['-I' + str(i) for i in includes],
                    str(here / 'check.cpp'), str(root / 'src/libs/island/src/map_zipper.cpp'), '-o', str(binary)],
                   check=True)
    subprocess.run([str(binary), str(Path(directory) / 'files')], check=True)
