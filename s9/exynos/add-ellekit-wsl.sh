#!/bin/bash
# Exynos iOS root + tweak injection: a copy of the clean root image with
# ElleKit (loader, TweakInject, libellekit) and the S9CISoft tweak.
#   bash s9/exynos/add-ellekit-wsl.sh [SRC.img [DST.img]]
# Needs s9/jailbreak/jb/debs/0-ellekit.deb (ElleKit 1.2, iphoneos-arm, from
# s9/jailbreak/fetch-jailbreak.sh) and s9/jailbreak/jb/S9CISoft.{dylib,plist}
# (s9-ios-tools CI artifact), plus what exynos/mkroot-wsl.sh needs for HFS+.
# Asks for sudo. SRC is not modified.
#
# Unlike the SDM845 root (s9/tools/prep-ios6.sh) there is no Procursus
# bootstrap, so no shell: only ElleKit's own files and one launchd job
# (com.ios-s9.ellekit: the loader at boot, s9/tools/jb-launchd.py) plus
# SpringBoard's DYLD_INSERT_LIBRARIES=/usr/lib/TweakInject.dylib. No respring
# job (it is a sh script): processes started after the loader get the tweaks,
# and a SpringBoard that crashes at start is restarted by launchd anyway.
set -euo pipefail
cd "$(dirname "$0")/.."
SRC=${1:-/mnt/f/ios-s9-ipsw/ios16-root-20H392-s9.img}
DST=${2:-/mnt/f/ios-s9-ipsw/ios16-root-20H392-s9-ellekit.img}
jb=jailbreak/jb
deb=$jb/debs/0-ellekit.deb
export PATH="$HOME/.local/bin:$PATH"

for f in "$SRC" "$deb" "$jb/S9CISoft.dylib" "$jb/S9CISoft.plist"; do
	[ -f "$f" ] || { echo "missing: $f" >&2; exit 1; }
done
[ "$SRC" != "$DST" ] || { echo "DST must differ from SRC" >&2; exit 1; }
ar t "$deb" | grep -qx data.tar.xz || { echo "$deb: no data.tar.xz" >&2; exit 1; }

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

echo "== ElleKit $(ar p "$deb" control.tar.xz | tar -xJ -O ./control | sed -n 's/^Version: //p')"
# ./etc only holds rc.d/ellekit-loader (the launchd job starts the loader)
ar p "$deb" data.tar.xz | sudo tar -xJp --numeric-owner --keep-directory-symlink \
	--exclude=./etc --exclude='./etc/*' -C "$mnt"

echo "== S9CISoft (CoreImage CPU renderer without Metal)"
# into usr/lib/TweakInject directly: /Library/MobileSubstrate/DynamicLibraries
# is ElleKit's absolute symlink to it, which would resolve on this PC
sudo install -d -m 755 "$mnt/usr/lib/TweakInject"
sudo install -m 755 -o 0 -g 0 "$jb/S9CISoft.dylib" "$mnt/usr/lib/TweakInject/S9CISoft.dylib"
sudo install -m 644 -o 0 -g 0 "$jb/S9CISoft.plist" "$mnt/usr/lib/TweakInject/S9CISoft.plist"
# the loader job logs here (launchd creates the file, not the directory)
sudo install -d -m 755 -o 0 -g 0 "$mnt/private/var/ios-s9"

echo "== launchd: ElleKit loader job + SpringBoard injection"
sudo python3 -I tools/jb-launchd.py "$mnt" com.ios-s9.ellekit

ls -la "$mnt/usr/libexec/ellekit" "$mnt/usr/lib/TweakInject" "$mnt/usr/lib/TweakInject.dylib"
sync
sudo umount "$mnt"
fsck.hfsplus -f -y "$DST" | grep -v 'Illegal name' || true
fsck.hfsplus -f -n "$DST"
sha256sum "$DST" | tee "$DST.sha256"
