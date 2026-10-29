#!/usr/bin/env python3
"""Print the nodes of a flattened device tree whose path or compatible matches
a regex (with --value: any property value), with all their properties
(strings / u32 cells / hex).

  fdtgrep.py DTB REGEX [--value]
"""
import re, struct, sys

FDT_BEGIN, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9


def nodes(b):
    magic, total, off_st, off_str = struct.unpack_from('>IIII', b, 0)
    assert magic == 0xd00dfeed
    pos, path, props = off_st, [], []
    stack = []
    while True:
        tok = struct.unpack_from('>I', b, pos)[0]
        pos += 4
        if tok == FDT_BEGIN:
            end = b.index(b'\0', pos)
            name = b[pos:end].decode()
            pos = (end + 4) & ~3
            stack.append((path, props))
            path, props = path + [name], []
        elif tok == FDT_PROP:
            ln, noff = struct.unpack_from('>II', b, pos)
            pos += 8
            pname = b[off_str + noff:b.index(b'\0', off_str + noff)].decode()
            props.append((pname, b[pos:pos + ln]))
            pos = (pos + ln + 3) & ~3
        elif tok == FDT_END_NODE:
            yield '/'.join(path) or '/', props
            path, props = stack.pop()
        elif tok == FDT_NOP:
            continue
        elif tok == FDT_END:
            return


def show(v):
    if v and v[-1] == 0 and all(32 <= c < 127 or c == 0 for c in v[:-1]):
        return ' / '.join(s.decode() for s in v[:-1].split(b'\0'))
    if len(v) % 4 == 0 and v:
        return '<' + ' '.join('0x%x' % x for x in struct.unpack('>%dI' % (len(v) // 4), v)) + '>'
    return v.hex()


b = open(sys.argv[1], 'rb').read()
pat = re.compile(sys.argv[2], re.I)
anyval = '--value' in sys.argv
for path, props in nodes(b):
    compat = dict(props).get('compatible', b'')
    hit = pat.search(path) or pat.search(compat.decode('latin1'))
    if anyval:
        hit = hit or any(pat.search(v.decode('latin1')) for _, v in props)
    if hit:
        print(path)
        for k, v in props:
            print('   ', k, '=', show(v))
