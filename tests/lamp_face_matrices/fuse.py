#!/usr/bin/env python3
"""Writes the lamp face check unit: check.cpp with the exact production face/submit fragments.

Run by the build (CMakeLists.txt next to this file): fuse.py <unit> <depfile>. The depfile lists every
file read, so Ninja re-fuses when a production source changes and tools/ci/affected.py sees the
sources in the build graph."""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[2]
unit_path, depfile = Path(sys.argv[1]), Path(sys.argv[2])
read_files = []


def read(path):
    read_files.append(path)
    return path.read_text()


def block(source, start):
    begin = source.index(start)
    end = source.index('{', begin) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end]


cpp = read(root / 'src/libs/renderer_bgfx/src/effects/lamp_shadows.cpp')
header = read(root / 'src/libs/renderer_bgfx/src/effects/lamp_shadows.hpp')
math = read(root / 'src/libs/renderer_bgfx/fx/src/lamp_shadows.cpp')
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
unit = read(Path(__file__).resolve().with_name('check.cpp'))
for marker, fragment in fragments.items():
    assert unit.count(marker) == 1
    unit = unit.replace(marker, fragment)
unit_path.write_text(unit)
depfile.write_text(str(unit_path) + ': ' + ' '.join(str(path).replace(' ', '\\ ') for path in read_files) + '\n')
