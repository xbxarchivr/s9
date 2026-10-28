#!/bin/bash
# Exynos S9 boot partition from TWRP, with the phone checked first.
#   exynos/twrp-boot.sh backup        -> backup/exynos-boot-stock.img (never overwritten)
#   exynos/twrp-boot.sh flash [IMG]   write IMG (default out/exynos/boot.img), read it
#                                     back, reboot only if the hashes match
#   exynos/twrp-boot.sh restore       write the stock backup back
# Refuses to touch anything that does not look like an Exynos starlte/star2lte:
# the Snapdragon S9 (starqlte) has its own scripts in tools/.
set -euo pipefail
cd "$(dirname "$0")/.."
# Git Bash on Windows would rewrite the phone's /dev and /tmp paths into C:/...
export MSYS_NO_PATHCONV=1
cmd=${1:-}
adb wait-for-recovery

dev=$(adb shell getprop ro.product.device | tr -d '\r')
hw=$(adb shell getprop ro.boot.hardware | tr -d '\r')
case "$dev" in
starlte|star2lte) ;;
*) echo "device '$dev' (hardware '$hw') is not starlte/star2lte: stopping" >&2; exit 1 ;;
esac
boot=$(adb shell 'for p in /dev/block/by-name/BOOT /dev/block/by-name/boot /dev/block/platform/*/by-name/boot /dev/block/platform/*/by-name/BOOT /dev/block/platform/*/*/by-name/boot; do [ -e $p ] && echo $p && break; done' | tr -d '\r')
[ -n "$boot" ] || { echo "boot partition not found" >&2; exit 1; }
part=$(adb shell "blockdev --getsize64 $boot" | tr -d '\r')
echo "$dev ($hw): boot = $boot, $part bytes"

mkdir -p backup
stock=backup/exynos-boot-stock.img

write() {
	local img=$1
	local n=$(( ($(stat -c %s "$img") + 4095) / 4096 ))
	[ $(( n * 4096 )) -le "$part" ] || { echo "$img is larger than the partition" >&2; exit 1; }
	adb push "$img" /tmp/x-boot.img
	adb shell "dd if=/tmp/x-boot.img of=$boot bs=4096 && sync"
	local want got
	want=$(sha256sum "$img" | cut -d' ' -f1)
	got=$(adb shell "dd if=$boot bs=4096 count=$n 2>/dev/null | head -c $(stat -c %s "$img") | sha256sum" | cut -d' ' -f1)
	[ "$want" = "$got" ] || { echo "readback $got != $want: NOT rebooting, flash again" >&2; exit 1; }
	echo "boot = $img ($want)"
}

case "$cmd" in
backup)
	[ ! -e "$stock" ] || { echo "$stock exists, keeping it" >&2; exit 1; }
	adb shell "dd if=$boot of=/tmp/x-stock.img bs=4096"
	adb pull /tmp/x-stock.img "$stock"
	head -c 8 "$stock" | grep -q 'ANDROID!' || { echo "warning: no ANDROID! magic in $stock" >&2; }
	sha256sum "$stock"
	;;
flash)
	[ -e "$stock" ] || { echo "no $stock: run 'backup' first" >&2; exit 1; }
	write "${2:-out/exynos/boot.img}"
	[ "${NOREBOOT:-0}" = 1 ] || adb reboot
	;;
restore)
	write "$stock"
	;;
*)
	echo "usage: exynos/twrp-boot.sh backup|flash [IMG]|restore" >&2; exit 2 ;;
esac
