#!/usr/bin/env python3
"""Run production animation cache lifetimes under ASan/UBSan in the kept build cache."""
import argparse
from pathlib import Path
import subprocess
import sys

here = Path(__file__).resolve().parent
sys.dont_write_bytecode = True
sys.path.insert(0, str(here.parent / 'glm_math'))
from glm_source import glm_source

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--glm-source', type=Path)
args = parser.parse_args()
build = args.build_dir.resolve()
subprocess.run(['cmake', '-S', str(here), '-B', str(build), '-G', 'Ninja',
                '-DCMAKE_BUILD_TYPE=Debug', '-DSTORM_GLM_SOURCE=' + str(glm_source(args.glm_source))], check=True)
subprocess.run(['cmake', '--build', str(build), '--target', 'animation_cache_check', '--parallel', '2'], check=True)
subprocess.run(['ctest', '--test-dir', str(build), '--output-on-failure'], check=True)
