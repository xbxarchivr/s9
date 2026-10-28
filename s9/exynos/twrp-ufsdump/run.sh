#!/sbin/sh
# On the phone (TWRP): a fresh 4 KB direct read so the UTRL holds a live
# data-in descriptor, then load ufsdump.ko (prints and refuses to stay).
dmesg -c > /dev/null
dd if=/dev/block/sda10 of=/dev/null bs=4096 count=1 iflag=direct 2>/dev/null || dd if=/dev/block/sda10 of=/dev/null bs=4096 count=1 skip=7 2>/dev/null
insmod /tmp/ufsdump.ko 2>&1
dmesg | grep UFSDUMP | sed 's/^.*UFSDUMP /UFSDUMP /'
