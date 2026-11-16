#!/usr/bin/env python3
"""Compare two script state images (STORM_VM_STATE_DIR dumps: the script part of a save, uncompressed) and name what
differs: the name codec, a variable's value, an attribute's path.

  python3 tools/vm_verify/state_diff.py A.state B.state [--limit N]

Exit 0 when the images are equal byte for byte."""
import argparse
from pathlib import Path
import struct
import sys

# S_TOKEN_TYPE values of the variable types, as every save stores them (src/libs/core/src/token.h).
INTEGER, FLOAT, STRING, OBJECT, REFERENCE, AREFERENCE, PTR = 6, 7, 8, 9, 10, 11, 12


class Reader:
    def __init__(self, data):
        self.data, self.at = data, 0

    def take(self, size):
        chunk = self.data[self.at:self.at + size]
        if len(chunk) != size:
            raise ValueError(f'truncated at byte {self.at}')
        self.at += size
        return chunk

    def vdword(self):
        first = self.take(1)[0]
        if first < 0xfe:
            return first
        return struct.unpack('<H', self.take(2))[0] if first == 0xfe else struct.unpack('<I', self.take(4))[0]

    def string(self):
        size = self.vdword()
        return None if size == 0 else self.take(size)[:-1].decode('cp1251', 'replace')


def attributes(reader, codec, path, out):
    count = reader.vdword()
    code = reader.vdword()
    value = reader.string()
    out[path] = value
    seen = {}
    for _ in range(count):
        mark = reader.at
        reader.vdword()
        child = reader.vdword()
        reader.at = mark
        name = codec.get(child, f'#{child}')
        index = seen.get(name, 0)  # an object may hold several attributes of one name
        seen[name] = index + 1
        attributes(reader, codec, f'{path}.{name}' + (f'#{index}' if index else ''), out)
    out[path + '/'] = ' '.join(f'{name}x{n}' if n > 1 else name for name, n in seen.items())  # the children's order


def value(reader, kind, codec, path, out):
    if kind == INTEGER:
        out[path] = struct.unpack('<i', reader.take(4))[0]
    elif kind == FLOAT:
        out[path] = struct.unpack('<f', reader.take(4))[0], reader.data[reader.at - 4:reader.at].hex()
    elif kind == PTR:
        out[path] = struct.unpack('<Q', reader.take(8))[0]
    elif kind == STRING:
        out[path] = reader.string()
    elif kind == OBJECT:
        out[path] = 'entity %d' % struct.unpack('<Q', reader.take(8))[0]
        attributes(reader, codec, path + ':', out)
    elif kind == REFERENCE:
        target = reader.vdword()
        out[path] = 'ref none' if target == 0xffffffff else f'ref {target}[{reader.vdword()}]'
    elif kind == AREFERENCE:
        target = reader.vdword()
        out[path] = 'aref none' if target == 0xffffffff else f'aref {target}[{reader.vdword()}].{reader.string()}'
    else:
        raise ValueError(f'{path}: unknown type {kind} at byte {reader.at}')


def parse(path):
    """{'codec': [names in saved order], 'values': {variable or attribute path: value}}."""
    reader = Reader(Path(path).read_bytes())
    state = {'program': reader.string(), 'codec': [reader.string() for _ in range(reader.vdword())]}
    # The codes as the loader assigns them: the names in saved order, numbered inside their bucket.
    codec, position = {}, {}
    for name in state['codec']:
        bucket = bucket_of(name.encode('cp1251', 'replace'))
        codec[(bucket << 16) | position.get(bucket, 0)] = name
        position[bucket] = position.get(bucket, 0) + 1
    state['segments'] = [reader.string() for _ in range(reader.vdword())]
    values = state['values'] = {}
    for _ in range(reader.vdword()):
        name = reader.string()
        kind, count = struct.unpack('<II', reader.take(8))
        if count > 1:
            for index in range(count):
                value(reader, kind, codec, f'{name}[{index}]', values)
        else:
            value(reader, kind, codec, name, values)
    if reader.at != len(reader.data):
        raise ValueError(f'{path}: {len(reader.data) - reader.at} bytes after the last variable')
    return state


def bucket_of(name):
    hval = 0
    for byte in name:
        if 65 <= byte <= 90:
            byte += 32
        if byte > 127:
            byte -= 256  # the engine's chars are signed
        hval = ((hval << 4) + byte) & 0xffffffff
        g = hval & 0xf0000000
        if g:
            hval ^= g >> 24
            hval ^= g
    return hval & 511


def differences(a, b, limit):
    lines = []
    for key in ('program', 'segments'):
        if a[key] != b[key]:
            lines.append(f'{key}: {a[key]!r} / {b[key]!r}')
    if a['codec'] != b['codec']:
        only_a, only_b = set(a['codec']) - set(b['codec']), set(b['codec']) - set(a['codec'])
        lines.append(f'name codec: {len(a["codec"])} / {len(b["codec"])} names; only in A: {sorted(only_a)[:8]}; '
                     f'only in B: {sorted(only_b)[:8]}' + ('' if only_a or only_b else '; same names in another order'))
    keys = list(a['values'])
    keys += [key for key in b['values'] if key not in a['values']]
    count = 0
    for key in keys:
        left, right = a['values'].get(key, '<absent>'), b['values'].get(key, '<absent>')
        if left != right:
            count += 1
            if count <= limit:
                lines.append(f'{key}: {left!r} / {right!r}')
    if count > limit:
        lines.append(f'... and {count - limit} more values')
    return lines


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('a', type=Path)
    parser.add_argument('b', type=Path)
    parser.add_argument('--limit', type=int, default=40)
    args = parser.parse_args()
    if args.a.read_bytes() == args.b.read_bytes():
        print('equal')
        sys.exit(0)
    lines = differences(parse(args.a), parse(args.b), args.limit)
    print('\n'.join(lines) if lines else 'the bytes differ, the parsed states do not')
    sys.exit(1)
