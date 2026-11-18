#!/usr/bin/env python3
"""A call whose argument count differs from the function's: the compile-time report of such calls in expressions
(COMPILER::CheckCallArguments) and the stack after one at run time (COMPILER::DropStrayStackValues), both extracted
verbatim from compiler.cpp and run against the real FuncInfo, S_STACK and DATA under ASan/UBSan; no engine build."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--dependency-sources', type=Path, required=True)
p.add_argument('--glm-source', type=Path, required=True)
a = p.parse_args()
r = Path(__file__).resolve().parents[2]
deps = a.dependency_sources.resolve()
source = (r/'src/libs/core/src/compiler.cpp').read_text()
methods = []
for name in ('void COMPILER::CheckCallArguments(', 'void COMPILER::DropStrayStackValues('):
    start = source.index(name)
    methods.append(source[start:source.index('\n}', start) + 2].replace('COMPILER::', 'Harness::'))

with tempfile.TemporaryDirectory(prefix='storm-call-arguments-') as directory:
    b = Path(directory)
    (b/'method.inc').write_text('\n'.join(methods))
    # data.cpp does not use CoreImpl; its include would instantiate the full-engine singleton.
    (b/'data.cpp').write_text((r/'src/libs/core/src/data.cpp').read_text().replace(
        '#include "core_impl.h"', '#include "platform/platform.hpp"\n#include <cmath>\nusing std::isnan;'))
    (b/'SDL2').symlink_to(deps/'SDL/include', target_is_directory=True)
    includes = list((r/'src/libs').glob('*/include')) + [r/'src/libs/core/src', b,
        deps/'fmt/include', deps/'spdlog/include', deps/'mimalloc/include',
        deps/'fast_float/include', r/'src/libs/renderer_bgfx/include', a.glm_source.resolve()]
    flags = ['-std=c++20', '-DFMT_HEADER_ONLY', '-DSPDLOG_FMT_EXTERNAL', '-g',
             '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
             '-fno-sanitize-recover=all'] + ['-I'+str(i) for i in includes]
    files = [Path(__file__).with_name('check.cpp'), b/'data.cpp', r/'src/libs/core/src/attributes.cpp',
             r/'src/libs/core/src/s_stack.cpp', r/'src/libs/core/src/s_functab.cpp']
    subprocess.run([os.environ.get('CXX', 'c++'), *flags, *map(str, files), '-o', str(b/'check')], check=True)
    subprocess.run([str(b/'check')], check=True)
