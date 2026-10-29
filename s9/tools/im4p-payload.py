#!/usr/bin/env python3
"""Extract the payload of an unencrypted, uncompressed IM4P (e.g. an IPSW
DeviceTree) without the ipsw tool:  im4p-payload.py IN.im4p OUT

IM4P = DER SEQUENCE { IA5String "IM4P", IA5String type, IA5String version,
OCTET STRING payload, ... }. Refuses LZFSE/LZSS-compressed or keyed payloads.
"""
import sys


def der(buf, off):
    """(tag, value start, value end) of the DER element at off."""
    tag, n = buf[off], buf[off + 1]
    off += 2
    if n & 0x80:
        k = n & 0x7f
        n = int.from_bytes(buf[off:off + k], 'big')
        off += k
    return tag, off, off + n


data = open(sys.argv[1], 'rb').read()
tag, start, end = der(data, 0)
assert tag == 0x30, 'not a DER SEQUENCE'
items, off = [], start
while off < end:
    t, s, e = der(data, off)
    items.append((t, data[s:e]))
    off = e
assert items[0] == (0x16, b'IM4P'), 'not an IM4P'
print('type', items[1][1].decode(), 'version', items[2][1].decode(), file=sys.stderr)
payload = items[3][1]
assert items[3][0] == 0x04, 'payload is not an OCTET STRING'
assert payload[:4] not in (b'bvx2', b'comp'), 'compressed payload'
assert len(items) < 5 or items[4][0] != 0x04, 'keyed (encrypted) payload'
open(sys.argv[2], 'wb').write(payload)
print(f'{len(payload)} bytes -> {sys.argv[2]}', file=sys.stderr)
