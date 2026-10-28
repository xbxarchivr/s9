#!/usr/bin/env python3
"""Put a new kernel into a copy of the phone's own boot.img.

    repack-boot.py STOCK_BOOT.img Image OUT.img [--ramdisk PAYLOAD]

Exynos S-Boot boot images differ between firmware releases (header v0 with
Samsung's dt_size in the header_version slot, v1 with recovery_dtbo, v2 with
dtb), and we have not seen the G960F/G965F one yet. So nothing is rebuilt:
the header keeps every field except kernel_size, the id and (v1) the
recovery_dtbo offset, and everything after the kernel pages (ramdisk, second,
dt, recovery_dtbo, dtb, Samsung's signature tail) is copied byte for byte.

The kernel is stored the way the stock one is: gzip if stock is gzip, and
any DTBs appended to the stock kernel are appended to ours too.

--ramdisk replaces the ramdisk as well (X4: the SWEETXNU payload rides there,
as on the SDM845); everything after it is shifted, still byte for byte.
"""
import gzip
import hashlib
import struct
import sys

FDT_MAGIC = b"\xd0\x0d\xfe\xed"


def align(n, page):
    return (n + page - 1) // page * page


def appended_dtbs(blob):
    """The chain of valid DTBs that ends exactly at the end of blob, or b''."""
    i = blob.find(FDT_MAGIC)
    while i >= 0:
        j = i
        while j + 40 <= len(blob) and blob[j:j + 4] == FDT_MAGIC:
            total, = struct.unpack(">I", blob[j + 4:j + 8])
            version, = struct.unpack(">I", blob[j + 20:j + 24])
            if total < 40 or version not in range(16, 18):
                break
            j += total
        if j == len(blob) and j > i:
            return blob[i:]
        i = blob.find(FDT_MAGIC, i + 1)
    return b""


def main():
    args = sys.argv[1:]
    ramdisk = None
    if "--ramdisk" in args:
        i = args.index("--ramdisk")
        ramdisk = open(args[i + 1], "rb").read()
        del args[i:i + 2]
    if len(args) != 3:
        sys.exit(__doc__)
    stock = open(args[0], "rb").read()
    image = open(args[1], "rb").read()
    if stock[:8] != b"ANDROID!":
        sys.exit("stock image has no ANDROID! magic")
    (k_size, _k_addr, r_size, _r_addr, s_size, _s_addr, _tags, page,
     hv) = struct.unpack_from("<9I", stock, 8)
    if hv in (3, 4):
        sys.exit("boot header v%d: not an S9 Exynos layout, refusing" % hv)
    if page not in (2048, 4096, 8192, 16384):
        sys.exit("odd page size %d" % page)

    old_kernel = stock[page:page + k_size]
    if old_kernel[:2] == b"\x1f\x8b":
        kernel = gzip.compress(image, 9, mtime=0)
        kind = "gzip"
    elif old_kernel[56:60] == b"ARM\x64":
        kernel = image
        kind = "raw Image"
    else:
        sys.exit("stock kernel is neither gzip nor a raw arm64 Image: look first")
    dtbs = appended_dtbs(old_kernel)
    kernel += dtbs

    old_end = page + align(k_size, page)
    new_end = page + align(len(kernel), page)
    delta = new_end - old_end
    hdr = bytearray(stock[:page])
    struct.pack_into("<I", hdr, 8, len(kernel))

    # Section layout after the kernel, for the id hash.
    tail = stock[old_end:]
    if ramdisk is not None:
        tail = ramdisk.ljust(align(len(ramdisk), page), b"\0") + tail[align(r_size, page):]
        delta += align(len(ramdisk), page) - align(r_size, page)
        r_size = len(ramdisk)
        struct.pack_into("<I", hdr, 16, r_size)
    r_off = 0
    s_off = r_off + align(r_size, page)
    extra = []
    if hv == 1 or hv == 2:
        rd_size, rd_off = struct.unpack_from("<IQ", stock, 1632)
        if rd_size:
            struct.pack_into("<Q", hdr, 1636, rd_off + delta)
            extra.append(stock[rd_off:rd_off + rd_size])
        if hv == 2:
            dtb_size, = struct.unpack_from("<I", stock, 1648)
            d_off = align(rd_off + rd_size, page) if rd_size else \
                old_end + s_off + align(s_size, page)
            extra.append(stock[d_off:d_off + dtb_size])
    elif hv:
        # Samsung's mkbootimg: header_version slot holds the dt image size.
        dt_off = s_off + align(s_size, page)
        extra.append(tail[dt_off:dt_off + hv])

    sha = hashlib.sha1()
    for part in [kernel, tail[r_off:r_off + r_size], tail[s_off:s_off + s_size]] + extra:
        sha.update(part)
        sha.update(struct.pack("<I", len(part)))
    hdr[576:608] = sha.digest().ljust(32, b"\0")

    out = bytes(hdr) + kernel.ljust(align(len(kernel), page), b"\0") + tail
    open(args[2], "wb").write(out)
    print("header v%s page %d, kernel %s %d -> %d bytes%s, image %d -> %d bytes"
          % (hv if hv <= 2 else "0+dt(%d)" % hv, page, kind, k_size, len(kernel),
             " (+%d bytes of appended DTBs)" % len(dtbs) if dtbs else "",
             len(stock), len(out)))


if __name__ == "__main__":
    main()
