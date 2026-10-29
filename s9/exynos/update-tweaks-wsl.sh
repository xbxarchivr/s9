#!/bin/bash
# Replace the tweaks inside the ElleKit Exynos root image, in place.
#   bash s9/exynos/update-tweaks-wsl.sh [IMG]
# Takes s9/jailbreak/jb/S9CISoft.{dylib,plist} (s9-ios-tools CI artifact).
# Then, phone in TWRP: python -I s9/exynos/write-userdata.py IMG --changed
# (writes only the 256 MiB chunks that differ from what USERDATA holds).
# Needs what exynos/add-ellekit-wsl.sh needs; asks for sudo.
set -euo pipefail
cd "$(dirname "$0")/.."
IMG=${1:-/mnt/f/ios-s9-ipsw/ios16-root-20H392-s9-ellekit.img}
jb=jailbreak/jb
export PATH="$HOME/.local/bin:$PATH"

for f in "$IMG" "$jb/S9CISoft.dylib" "$jb/S9CISoft.plist"; do
	[ -f "$f" ] || { echo "missing: $f" >&2; exit 1; }
done
grep -qw hfsplus /proc/filesystems || sudo insmod "$HOME/wslk/k/fs/hfsplus/hfsplus.ko"

mnt=$(mktemp -d)
cleanup() {
	mountpoint -q "$mnt" && sudo umount "$mnt"
	rmdir "$mnt" 2>/dev/null || true
}
trap cleanup EXIT
sudo mount -t hfsplus -o loop,rw,force "$IMG" "$mnt"
[ -e "$mnt/usr/libexec/ellekit/loader" ] || { echo "$IMG has no ElleKit (run add-ellekit-wsl.sh)" >&2; exit 1; }

echo "== S9CISoft"
sudo install -m 755 -o 0 -g 0 "$jb/S9CISoft.dylib" "$mnt/usr/lib/TweakInject/S9CISoft.dylib"
sudo install -m 644 -o 0 -g 0 "$jb/S9CISoft.plist" "$mnt/usr/lib/TweakInject/S9CISoft.plist"
# its log (SpringBoard and other unsandboxed processes running as mobile)
sudo install -d -m 755 -o 501 -g 501 "$mnt/private/var/mobile/Library/S9CISoft"
sha256sum "$mnt/usr/lib/TweakInject/S9CISoft.dylib" "$mnt/usr/lib/TweakInject/S9CISoft.plist"
sync
sudo umount "$mnt"
fsck.hfsplus -f -y "$IMG" | grep -v 'Illegal name' || true
fsck.hfsplus -f -n "$IMG"
sha256sum "$IMG" | tee "$IMG.sha256"
