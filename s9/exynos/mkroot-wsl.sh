#!/bin/bash
# Build the S9 iOS 16 root (HFS+, for USERDATA) under WSL2 on Windows.
#   bash s9/exynos/mkroot-wsl.sh [OUT.img]
# Needs (NOTES.md "iOS root on WSL"): hfsprogs, rsync, ~/.local/bin/apfs-fuse,
# and ~/wslk/k/fs/hfsplus/hfsplus.ko built for the running WSL kernel (the
# stock WSL2 kernel has no HFS+). The three IPSW images (iPhone10,3/10,6
# 16.7.16 20H392, tools/remotezip.py) are in $IPSW. Asks for sudo.
set -euo pipefail
cd "$(dirname "$0")/../.."
IPSW=${IPSW:-/mnt/f/ios-s9-ipsw/20H392}
OUT=${1:-/mnt/f/ios-s9-ipsw/ios16-root-20H392-s9.img}
export PATH="$HOME/.local/bin:$PATH"

grep -qw hfsplus /proc/filesystems || sudo insmod "$HOME/wslk/k/fs/hfsplus/hfsplus.ko"
grep -w hfsplus /proc/filesystems

bash tools/mkiosroot.sh "$IPSW/087-86698-044.dmg" "$IPSW/087-86593-047.dmg" \
	"$IPSW/087-86715-047.dmg" "$OUT"

# The SWEET bring-up changes (launchd, containers, Setup skip, activation...)
mnt=$(mktemp -d)
sudo mount -t hfsplus -o loop,force "$OUT" "$mnt"
sudo python3 tools/sweet-root-tweaks.py "$mnt"
sync
sudo umount "$mnt"
rmdir "$mnt"
fsck.hfsplus -f -y "$OUT" | grep -v 'Illegal name' || true
fsck.hfsplus -f -n "$OUT"
sha256sum "$OUT" | tee "$OUT.sha256"
