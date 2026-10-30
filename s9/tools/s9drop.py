#!/usr/bin/env python3
"""Install files on the Exynos phone's iOS and run one, from TWRP (ios-tools/s9drop.c).

  s9drop.py push [--run "CMD ARGS"] LOCAL:/ios/path[:MODE] ...
  s9drop.py result

push writes a package to USERDATA at 16 GiB, past the HFS+ root and before the
sweet log at the partition's end; only that range is touched. Reboot into iOS
and it runs once per push. result prints what came back, from TWRP.
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import time

PART = '/dev/block/sda25'
BLOCK = 4096
DROP_OFF = 16 << 30
RES_OFF = DROP_OFF + (240 << 20)
RES_MAX = (16 << 20) - BLOCK
MAX_FILES = 16
FILE_ENTRY = '<128sQQII'
HDR = '<8sII512s'
RES = '<8sIiII256s'


def adb(*args, capture=True):
    r = subprocess.run(['adb', *args], capture_output=capture, text=True)
    if r.returncode != 0:
        sys.exit(f'adb {" ".join(args)}: {r.stderr.strip()}')
    return (r.stdout or '').strip()


def check_part():
    """Same device and a size that leaves room, or nothing gets written."""
    link = adb('shell', f'readlink -f /dev/block/by-name/USERDATA || readlink -f /dev/block/platform/*/by-name/USERDATA')
    if link != PART:
        sys.exit(f'USERDATA is {link!r}, expected {PART}')
    sectors = int(adb('shell', 'cat /sys/class/block/sda25/size'))
    if sectors * 512 < RES_OFF + RES_MAX + BLOCK + (8 << 20):
        sys.exit(f'{PART} is too small: {sectors} sectors')


def pad(data):
    return data + b'\0' * (-len(data) % BLOCK)


def build(files, run, generation):
    entries = b''
    body = b''
    for local, path, mode in files:
        data = open(local, 'rb').read()
        raw = path.encode()
        if not path.startswith('/') or len(raw) >= 128:
            sys.exit(f'bad path {path}')
        entries += struct.pack(FILE_ENTRY, raw, BLOCK + len(body), len(data), mode, 0)
        body += pad(data)
    entries += b'\0' * (struct.calcsize(FILE_ENTRY) * (MAX_FILES - len(files)))
    head = struct.pack(HDR, b'S9DROP1\n', generation, len(files), run.encode()) + entries
    if len(head) > BLOCK:
        sys.exit('header too big')
    pkg = pad(head) + body
    if len(pkg) > RES_OFF - DROP_OFF:
        sys.exit('package too big')
    return pkg


def push(args):
    files = []
    for spec in args.files:
        # LOCAL may hold a drive letter: split from the right, on ":/"
        local, sep, rest = spec.rpartition(':/')
        if not sep:
            sys.exit(f'bad file {spec}, want LOCAL:/ios/path[:MODE]')
        path, _, mode = ('/' + rest).partition(':')
        files.append((local, path, int(mode or '755', 8)))
    if len(files) > MAX_FILES:
        sys.exit(f'at most {MAX_FILES} files')
    if len(args.run.encode()) >= 512:
        sys.exit('--run too long')
    check_part()
    pkg = build(files, args.run, int(time.time()))
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, 'drop.bin')
        open(p, 'wb').write(pkg)
        adb('push', p, '/tmp/s9drop.bin')
    want = hashlib.sha256(pkg).hexdigest()
    seek = DROP_OFF // BLOCK
    adb('shell', f'dd if=/tmp/s9drop.bin of={PART} bs={BLOCK} seek={seek} conv=notrunc 2>/dev/null; sync')
    got = adb('shell', f'dd if={PART} bs={BLOCK} skip={seek} count={len(pkg) // BLOCK} 2>/dev/null | sha256sum').split()[0]
    if got != want:
        sys.exit(f'readback {got} != {want}')
    print(f'package {len(pkg)} bytes at {DROP_OFF}, readback ok {want[:16]}')
    print(f'run: {args.run or "(nothing)"}')


def result(_):
    check_part()
    skip = RES_OFF // BLOCK
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, 'res.bin')
        adb('shell', f'dd if={PART} of=/tmp/s9res.bin bs={BLOCK} skip={skip} count=1 2>/dev/null')
        adb('pull', '/tmp/s9res.bin', p)
        raw = open(p, 'rb').read()
        magic, gen, status, length, state, note = struct.unpack_from(RES, raw)
        if magic != b'S9RES01\n':
            sys.exit('no result: s9drop never ran, or could not write')
        note = note.split(b'\0')[0].decode(errors='replace')
        names = {1: 'installing', 2: 'running', 3: 'done'}
        print(f'generation {gen} state {names.get(state, state)} status {status} length {length} note {note!r}')
        if 0 < length <= RES_MAX:
            n = (length + BLOCK - 1) // BLOCK
            adb('shell', f'dd if={PART} of=/tmp/s9res.bin bs={BLOCK} skip={skip + 1} count={n} 2>/dev/null')
            adb('pull', '/tmp/s9res.bin', p)
            sys.stdout.write(open(p, 'rb').read()[:length].decode(errors='replace'))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest='cmd', required=True)
    a = sub.add_parser('push')
    a.add_argument('--run', default='')
    a.add_argument('files', nargs='*')
    a.set_defaults(func=push)
    sub.add_parser('result').set_defaults(func=result)
    args = ap.parse_args()
    args.func(args)


if __name__ == '__main__':
    main()
