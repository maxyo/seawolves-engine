#!/usr/bin/env python3
"""Exact production face/submit fragments with pinned bx math; no renderer/device link."""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--bx-source', type=Path, required=True, help='Offline pinned bx source')
parser.add_argument('--scratch', type=Path, required=True, help='Task-owned temporary directory parent')
args = parser.parse_args()


def block(source, start):
    begin = source.index(start)
    end = source.index('{', begin) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end]


cpp = (root / 'src/libs/renderer_bgfx/src/effects/lamp_shadows.cpp').read_text()
header = (root / 'src/libs/renderer_bgfx/src/effects/lamp_shadows.hpp').read_text()
math = (root / 'src/libs/renderer_bgfx/fx/src/lamp_shadows.cpp').read_text()
prepare = block(cpp, 'bool LampShadows::prepare(')
reset = re.search(r'for \(Slot &slot : slot_\)\s*slot.dynamic_faces_ready = 0;', prepare).group()
assert prepare.index('if (prepared_)') < prepare.index('choose_moving(eye);') < prepare.index(reset)
assert prepare.index(reset) < prepare.index('draw_static(device, all, casters);')
fields = '\n'.join(re.findall(r'        (std::array<std::array<float, 16>, 6> dynamic_faces\{\};|uint8_t dynamic_faces_ready = 0;)', header))
assert fields.count(';') == 2
submit = block(cpp, 'void LampShadows::submit_caster(')
assert submit.index('SphereInLampFace') < submit.index('dynamic_face_matrix(s, face, in_faces)')
old = submit.replace('LampShadows::submit_caster(', 'LampShadows::submit_uncached(').replace(
    'const std::array<float, 16> &m = dynamic_face_matrix(s, face, in_faces);',
    'const std::array<float, 16> m = face_matrix(in_faces, s, face, columns_, rows_);')
assert old != submit and 'dynamic_face_matrix' not in old
face = block(cpp, 'std::array<float, 16> face_matrix(')
face = face.replace('{', '{\n    ++matrix_builds;', 1)  # observe actual projection builds
fragments = {
    '// FX_MATH': '\n'.join(block(math, signature) for signature in (
        'float Dot(', 'const std::array<CubeFace, 6> &CubeFaces(',
        'std::array<float, 16> LampFaceViewProj(', 'bool SphereInLampFace(',
        'AtlasTile LampAtlasTile(', 'std::array<float, 16> TileClip(')),
    '// CACHE_FIELDS': fields,
    '// RESET': reset,
    '// PRODUCTION': '\n'.join((block(cpp, 'float distance('), face, block(cpp, 'void scissor_tile('),
        block(cpp, 'const std::array<float, 16> &LampShadows::dynamic_face_matrix('), submit, old)),
}
unit = Path(__file__).with_name('check.cpp').read_text()
for marker, fragment in fragments.items():
    assert unit.count(marker) == 1
    unit = unit.replace(marker, fragment)
with tempfile.TemporaryDirectory(prefix='lamp-face-', dir=args.scratch) as directory:
    source, binary = Path(directory) / 'check.cpp', Path(directory) / 'check'
    source.write_text(unit)
    subprocess.run([*shlex.split(os.environ.get('CXX', 'c++')), '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror',
        '-DBX_CONFIG_DEBUG=0', '-I' + str(args.bx_source / 'include'),
        '-I' + str(root / 'src/libs/renderer_bgfx/fx/include'), str(source), '-o', str(binary)],
        env={**os.environ, 'TMPDIR': directory}, check=True)
    subprocess.run([str(binary)], check=True)
    for mutation in ('no-reset', 'uncached'):
        result = subprocess.run([str(binary), mutation], capture_output=True, text=True)
        if result.returncode != 1 or 'FAIL:' not in result.stderr:
            raise RuntimeError('Mutation was not rejected: ' + mutation + result.stderr)
        print('PASS rejected ' + mutation + ': ' + result.stderr.strip())
