#!/bin/bash
# Build ufsdump.ko against Samsung OSRC 4.9.118 + TWRP's config, with the
# symbol CRCs and release string of the running TWRP kernel.
set -e
SP=/mnt/c/Users/cipi/AppData/Local/Temp/claude/C--Users-cipi-Downloads-ios-s9/53695012-099f-4dbb-83a7-19b0e480e2aa/scratchpad
K=~/twrp-mod/k118
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- ANDROID_MAJOR_VERSION=q ANDROID_VERSION=100000 PLATFORM_VERSION=10
# CRCs read from TWRP's Image (kcrc2.py); printk/__ioremap/__iounmap/module_layout
# also reproduced by genksyms on this tree
printf '0x599efa6b\tmodule_layout\tvmlinux\tEXPORT_SYMBOL\n0x27e1a049\tprintk\tvmlinux\tEXPORT_SYMBOL\n0xf24b3dfe\t__ioremap\tvmlinux\tEXPORT_SYMBOL\n0x45a55ec8\t__iounmap\tvmlinux\tEXPORT_SYMBOL\n0xfadf2436\tmemstart_addr\tvmlinux\tEXPORT_SYMBOL\n' > $K/Module.symvers
echo '#define UTS_RELEASE "4.9.118-g36e6ddff22b9"' > $K/include/generated/utsrelease.h
echo 4.9.118-g36e6ddff22b9 > $K/include/config/kernel.release
rm -rf ~/twrp-mod/ufsdump && mkdir -p ~/twrp-mod/ufsdump
tr -d '\r' < $SP/mod/ufsdump.c > ~/twrp-mod/ufsdump/ufsdump.c
tr -d '\r' < $SP/mod/Makefile > ~/twrp-mod/ufsdump/Makefile
make -s -C $K M=$HOME/twrp-mod/ufsdump HOSTCFLAGS=-fcommon modules 2>&1 | grep -v '^$' || true
cd ~/twrp-mod/ufsdump
aarch64-linux-gnu-readelf -p .modinfo ufsdump.ko | grep -E 'vermagic|depends'
aarch64-linux-gnu-nm -u ufsdump.ko
modprobe --dump-modversions ufsdump.ko 2>/dev/null || true
cp ufsdump.ko $SP/mod/ufsdump.ko
