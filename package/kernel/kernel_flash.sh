#!/sbin/sh
cd /tmp/
/tmp/dd if=/dev/block/platform/msm_sdcc.1/by-name/boot of=/tmp/boot.img
/tmp/unpackbootimg /tmp/boot.img
/tmp/mkbootimg --kernel /tmp/zImage --ramdisk /tmp/boot.img-ramdisk.gz --cmdline 'androidboot.hardware=qcom user_debug=31' --base 0x80200000  --ramdiskaddr 0x81500000 -o /tmp/newboot.img
/tmp/dd if=/tmp/newboot.img of=/dev/block/platform/msm_sdcc.1/by-name/boot
busybox chmod 644 /system/lib/modules/*
