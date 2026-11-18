#!/usr/bin/env python3
"""The bone indices (a_indices) of every compiled Vulkan vertex shader are an unsigned integer input, without a GPU.

bgfx binds the non-normalised Uint8 indices as VK_FORMAT_R8G8B8A8_UINT (and GL_UNSIGNED_BYTE through
glVertexAttribIPointer). A signed `ivec4` input does not match the format's numeric type: undefined input
(VUID-VkGraphicsPipelineCreateInfo-Input-08733); PowerVR's Vulkan driver drew characters without legs. Reads the
SPIR-V of the build's shader blobs (shaders-gen/storm_shaders/vs_*.spv.bin, bgfx's shader binary format).
"""
import struct
import sys
from pathlib import Path

SPIRV_MAGIC = 0x07230203
OP_NAME, OP_TYPE_INT, OP_TYPE_VECTOR, OP_TYPE_POINTER, OP_VARIABLE = 5, 21, 23, 32, 59
STORAGE_INPUT = 1


def spirv_of(blob):
    """The SPIR-V module of a bgfx shader binary: the code follows its uint32 size."""
    start = blob.find(struct.pack('<I', SPIRV_MAGIC))
    if start < 4:
        raise ValueError('no SPIR-V module')
    size = struct.unpack_from('<I', blob, start - 4)[0]
    return struct.unpack_from(f'<{size // 4}I', blob, start)


def input_types(words):
    """{input variable name: (component type, signed)} of a SPIR-V module."""
    names, ints, vectors, pointers, inputs = {}, {}, {}, {}, {}
    i = 5
    while i < len(words):
        count, op = words[i] >> 16, words[i] & 0xFFFF
        args = words[i + 1:i + count]
        if op == OP_NAME:
            raw = struct.pack(f'<{len(args) - 1}I', *args[1:])
            names[args[0]] = raw.split(b'\0', 1)[0].decode()
        elif op == OP_TYPE_INT:
            ints[args[0]] = (args[1], args[2])
        elif op == OP_TYPE_VECTOR:
            vectors[args[0]] = args[1]
        elif op == OP_TYPE_POINTER:
            pointers[args[0]] = (args[1], args[2])
        elif op == OP_VARIABLE and args[2] == STORAGE_INPUT:
            inputs[args[1]] = args[0]
        i += max(count, 1)
    result = {}
    for variable, pointer in inputs.items():
        _, pointee = pointers[pointer]
        component = vectors.get(pointee, pointee)
        result[names.get(variable, f'%{variable}')] = ints.get(component)
    return result


def main():
    if len(sys.argv) != 2:
        sys.exit(f'usage: {sys.argv[0]} <build>/renderer_bgfx/shaders-gen/storm_shaders')
    shaders = sorted(Path(sys.argv[1]).glob('vs_*.spv.bin'))
    if not shaders:
        sys.exit(f'FAIL: no compiled vertex shaders (vs_*.spv.bin) in {sys.argv[1]}')
    failures, checked = [], 0
    for path in shaders:
        types = input_types(spirv_of(path.read_bytes()))
        if 'a_indices' not in types:
            continue
        checked += 1
        integer = types['a_indices']
        if integer is None or integer[1] != 0:
            failures.append(f'{path.name}: a_indices is {"signed int" if integer else "not an integer"}, the format '
                            'is VK_FORMAT_R8G8B8A8_UINT (varying.def.sc: uvec4)')
    if checked == 0:
        failures.append('no vertex shader declares a_indices: the skinned shaders were not found')
    for failure in failures:
        print('FAIL', failure)
    if failures:
        sys.exit(1)
    print(f'PASS: a_indices is unsigned in all {checked} skinned vertex shaders')


main()
