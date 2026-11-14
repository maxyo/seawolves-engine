#!/usr/bin/env python3
"""Check the production FF vertex SPIR-V exports and writes the actual PointSize built-in, without a GPU.

--inject-patch is a pre-build check: use an existing pinned compiler, but apply precisely the output field from
our shaderc patch to its preprocessed HLSL. Omit it to validate the rebuilt production shaderc end to end.
The bare-PSIZE negative control must compile but have no PointSize built-in.
"""
import argparse
import ast
from pathlib import Path
import re
import struct
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--shaderc', type=Path, required=True)
p.add_argument('--bgfx-include', type=Path, required=True)
p.add_argument('--inject-patch', action='store_true')
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
renderer = root / 'src/libs/renderer_bgfx'
field = r'(?:\[\[vk::builtin\("PointSize"\)\]\] )?float gl_PointSize : PSIZE;'
patch = (renderer / 'patches/bgfx-shaderc-point-size.patch').read_text()
line = next(x[1:].strip() for x in patch.splitlines() if x.startswith('+') and 'float gl_PointSize : PSIZE;' in x)
patched_field = ast.literal_eval(line).strip()

def run(*args):
    subprocess.run([str(a.shaderc), '--type', 'vertex', '--platform', 'android', '-p', 'spirv',
        '--varyingdef', str(renderer / 'shaders/varying.def.sc'), *map(str, args)], check=True)

def point_size_stored(path):
    binary = path.read_bytes()
    offset = binary.index(b'\x03\x02\x23\x07')
    # bgfx wraps SPIR-V with its header/reflection and a trailing sentinel. Walk instructions through
    # the function end instead of treating the surrounding container bytes as SPIR-V instructions.
    words = struct.unpack('<' + 'I' * ((len(binary)-offset)//4), binary[offset:offset+(len(binary)-offset)//4*4])
    at, builtins, outputs, stores = 5, set(), set(), set()
    while at < len(words):
        count, opcode = words[at] >> 16, words[at] & 65535
        if not count or at+count > len(words): break
        args = words[at+1:at+count]
        if opcode == 71 and len(args) == 3 and args[1:] == (11, 1): builtins.add(args[0])  # OpDecorate BuiltIn PointSize
        if opcode == 59 and args[2] == 3: outputs.add(args[1])  # OpVariable Output
        if opcode == 62: stores.add(args[0])  # OpStore
        at += count
    return bool(builtins & outputs & stores)

with tempfile.TemporaryDirectory(prefix='storm-pointsize-') as directory:
    work = Path(directory)
    for variant in ('vs_ff', 'vs_ff_unlit'):
        hlsl = work / (variant + '.hlsl')
        run('--preprocess', '-f', renderer / f'shaders/{variant}.sc', '-o', hlsl,
            '-i', a.bgfx_include, '-i', renderer / 'shaders/include', '-i', renderer / 'fx/shaders/include')
        source = hlsl.read_text()
        assert len(re.findall(field, source)) == 1, 'production FF shader PointSize field missing or duplicated'
        if a.inject_patch:
            source = re.sub(field, lambda _: patched_field, source)
        for control, text, expected in [('production', source, True),
                ('bare-PSIZE', re.sub(field, 'float gl_PointSize : PSIZE;', source), False)]:
            hlsl.write_text(text)
            output = work / (variant + '-' + control + '.bin')
            run('--raw', '-f', hlsl, '-o', output)
            actual = point_size_stored(output)
            assert actual == expected, f'{variant}/{control}: stored PointSize built-in={actual}, expected {expected}'
            print(f'PASS {variant}/{control}: stored PointSize built-in={actual}', flush=True)
print('PointSize SPIR-V check PASS' + (' (pre-build field injection only)' if a.inject_patch else ' (production compiler)'))
