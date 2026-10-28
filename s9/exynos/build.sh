#!/bin/bash
# Build an Exynos (SM-G960F/G965F) shim stage and put it into a copy of the
# phone's own stock boot.img (tools: backup-boot.sh fetches it from TWRP).
#   ./build.sh [x1]                     stage to build (default x1)
#   STOCK_BOOT=path ./build.sh          base boot.img (default backup/exynos-boot-stock.img)
#   out/exynos/boot.img      -> exynos/flash-twrp.sh (verified dd)
#   out/exynos/boot.tar.md5  -> Odin, AP slot
# The shared code (entry, framebuffer, console, DT) lives in ../shim.
set -euo pipefail
cd "$(dirname "$0")/.."
CROSS=${CROSS_COMPILE:-aarch64-linux-gnu-}
CC=${CROSS}gcc
OBJCOPY=${CROSS}objcopy
READELF=${CROSS}readelf
NM=${CROSS}nm
STOCK_BOOT=${STOCK_BOOT:-backup/exynos-boot-stock.img}
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -fno-tree-loop-distribute-patterns -Wall -Wextra -Ishim"

STAGE=${1:-x1}
case $STAGE in
x1) main="exynos/x1.c exynos/xentry.S" ;;
*) echo "usage: exynos/build.sh [x1]" >&2; exit 2 ;;
esac

out=out/exynos
mkdir -p $out/obj
rm -f $out/obj/*.o
objs=()
for f in shim/start.S $main shim/fb.c shim/con.c shim/fdt.c shim/lib.c; do
	o=$out/obj/$(basename "${f%.*}").o
	"$CC" $CFLAGS -c "$f" -o "$o"
	objs+=("$o")
done
# S-Boot jumps to wherever it put the kernel: position-independent code only.
abs=$("$READELF" -r "${objs[@]:1}" | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
"$CC" $CFLAGS -static -T shim/shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	"${objs[@]}" -o $out/shim.elf
"$OBJCOPY" -O binary $out/shim.elf $out/Image
size=$("$NM" $out/shim.elf | awk '$3=="_end"{print strtonum("0x"$1)}')
truncate -s "$size" $out/Image
ls -l $out/Image

if [ ! -f "$STOCK_BOOT" ]; then
	echo "no $STOCK_BOOT: Image only. Run exynos/twrp-boot.sh backup with the phone in TWRP first." >&2
	exit 0
fi
python3 -I exynos/repack-boot.py "$STOCK_BOOT" $out/Image $out/boot.img
(cd $out && tar -H ustar -cf boot.tar boot.img && md5sum -t boot.tar >> boot.tar && mv boot.tar boot.tar.md5)
cp $out/boot.img "$out/boot-$STAGE.img"
ls -l $out/boot.img $out/boot.tar.md5
sha256sum $out/boot.img
