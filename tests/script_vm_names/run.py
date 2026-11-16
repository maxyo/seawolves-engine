#!/usr/bin/env python3
"""Script attribute names against their plain implementations: STRING_CODEC against the codec the saves were numbered
with, ATTRIBUTES against a tree that compares names one by one; built with the VM's own checks (STORM_VM_VERIFY)."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--dependency-sources', type=Path, required=True)
args = parser.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[1]
deps = args.dependency_sources.resolve()
core = root / 'src/libs/core'
with tempfile.TemporaryDirectory(prefix='storm-script-vm-names-') as directory:
    binary = Path(directory) / 'check'
    subprocess.run([
        os.environ.get('CXX', 'c++'), '-std=c++20', '-O1', '-g', '-DSTORM_VM_VERIFY=1', '-DFMT_HEADER_ONLY',
        '-DSPDLOG_FMT_EXTERNAL', '-Wno-deprecated-declarations', '-fsanitize=address,undefined',
        '-fno-omit-frame-pointer', '-fno-sanitize-recover=all',
        f'-I{core}/include', f'-I{core}/src', f'-I{root}/src/libs/util/include', f'-I{deps}/fmt/include',
        f'-I{deps}/spdlog/include', f'-I{here}',
        str(here / 'check.cpp'), str(core / 'src/attributes.cpp'), str(core / 'src/vm_verify.cpp'), '-o', str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
