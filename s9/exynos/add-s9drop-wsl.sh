#!/bin/bash
# Exynos iOS root + s9drop: a copy of the ElleKit root image with the launchd
# job that installs and runs packages from USERDATA past the root
# (ios-tools/s9drop.c, tools/s9drop.py). Only this one image write is needed;
# later tools go through tools/s9drop.py from TWRP.
#   bash s9/exynos/add-s9drop-wsl.sh [SRC.img [DST.img]]
# Takes s9/jailbreak/jb/s9drop (s9-ios-tools CI artifact).
# Then, phone in TWRP: python -I s9/exynos/write-userdata.py DST --changed
# Needs what exynos/add-ellekit-wsl.sh needs; asks for sudo. SRC is not modified.
set -euo pipefail
cd "$(dirname "$0")/.."
SRC=${1:-/mnt/f/ios-s9-ipsw/ios16-root-20H392-s9-ellekit.img}
DST=${2:-/mnt/f/ios-s9-ipsw/ios16-root-20H392-s9-drop.img}
jb=jailbreak/jb
export PATH="$HOME/.local/bin:$PATH"

for f in "$SRC" "$jb/s9drop"; do
	[ -f "$f" ] || { echo "missing: $f" >&2; exit 1; }
done
[ "$SRC" != "$DST" ] || { echo "DST must differ from SRC" >&2; exit 1; }
grep -qw hfsplus /proc/filesystems || sudo insmod "$HOME/wslk/k/fs/hfsplus/hfsplus.ko"

echo "== copy $SRC -> $DST"
cp --sparse=always "$SRC" "$DST"

mnt=$(mktemp -d)
cleanup() {
	mountpoint -q "$mnt" && sudo umount "$mnt"
	rmdir "$mnt" 2>/dev/null || true
}
trap cleanup EXIT
sudo mount -t hfsplus -o loop,rw,force "$DST" "$mnt"
[ -e "$mnt/usr/libexec/ellekit/loader" ] || { echo "$SRC has no ElleKit (run add-ellekit-wsl.sh)" >&2; exit 1; }

echo "== s9drop"
sudo install -m 755 -o 0 -g 0 "$jb/s9drop" "$mnt/usr/libexec/s9drop"
sudo python3 -I tools/jb-launchd.py "$mnt" com.ios-s9.s9drop
sha256sum "$mnt/usr/libexec/s9drop"
sync
sudo umount "$mnt"
fsck.hfsplus -f -y "$DST" | grep -v 'Illegal name' || true
fsck.hfsplus -f -n "$DST"
sha256sum "$DST" | tee "$DST.sha256"
