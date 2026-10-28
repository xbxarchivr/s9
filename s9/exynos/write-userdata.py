#!/usr/bin/env python3
"""Write the iOS HFS+ root image to the S9's USERDATA from TWRP, verified.

  python -I s9/exynos/write-userdata.py IMAGE [START_CHUNK]

TWRP 3.7's adb exec-in is not binary-safe, so the image goes in 256 MiB
chunks: adb push to /tmp (RAM), dd to its offset in USERDATA, read that range
back and compare sha256 before the next one. Then the whole written range is
read back and compared with the image's sha256. Refuses anything but a
starlte whose USERDATA is sda25, unmounted, and big enough. DESTRUCTIVE:
overwrites the start of USERDATA (Android's data).
"""
import hashlib, os, subprocess, sys

CHUNK = 256 << 20
BS = 4 << 20
PART = '/dev/block/sda25'


def adb(*args, check=True):
    return subprocess.run(['adb', *args], check=check, capture_output=True, text=True).stdout.strip()


def sh(cmd):
    return adb('shell', cmd)


img, start = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 0
size = os.path.getsize(img)
assert size % CHUNK == 0, 'image size must be a multiple of 256 MiB'
assert sh('getprop ro.product.device') == 'starlte', 'not a starlte'
assert sh('readlink -f /dev/block/by-name/USERDATA') == PART, 'USERDATA is not sda25'
assert int(sh(f'blockdev --getsize64 {PART}')) >= size, 'USERDATA too small'
assert 'sda25' not in sh('cat /proc/mounts'), 'USERDATA is mounted'

n = size // CHUNK
tmp = img + '.chunk'
with open(img, 'rb') as f:
    f.seek(start * CHUNK)
    for i in range(start, n):
        data = f.read(CHUNK)
        want = hashlib.sha256(data).hexdigest()
        with open(tmp, 'wb') as c:
            c.write(data)
        adb('push', tmp, '/tmp/c.bin')
        seek = i * (CHUNK // BS)
        sh(f'dd if=/tmp/c.bin of={PART} bs={BS} seek={seek} conv=fsync 2>/dev/null; rm /tmp/c.bin')
        got = sh(f'dd if={PART} bs={BS} skip={seek} count={CHUNK // BS} 2>/dev/null | sha256sum').split()[0]
        if got != want:
            sys.exit(f'chunk {i}: readback {got} != {want}; rerun with START_CHUNK={i}')
        print(f'chunk {i + 1}/{n} ok', flush=True)
os.remove(tmp)

whole = hashlib.sha256()
with open(img, 'rb') as f:
    for blk in iter(lambda: f.read(BS * 16), b''):
        whole.update(blk)
got = sh(f'dd if={PART} bs={BS} count={size // BS} 2>/dev/null | sha256sum').split()[0]
print('image  ', whole.hexdigest())
print('USERDATA', got)
sys.exit(0 if got == whole.hexdigest() else 'MISMATCH over the whole image')
