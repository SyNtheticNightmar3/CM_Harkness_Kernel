#!/bin/bash

# export USE_CCACHE=1;
# export CCACHE_DIR=.ccache;
# ccache -M 2G;

export ARCH=arm;
export CROSS_COMPILE=~/gcc-toolchains/arm-none-eabi-4.8.3/bin/arm-none-eabi-;

export PATH=~/gcc-toolchains/arm-none-eabi-4.8.3/bin/cache:~/gcc-toolchains/arm-none-eabi-4.8.3/bin:$PATH;
export CS_GCC_TRIPLET=arm-none-eabi-;

while getopts ":c :p" opt
do
case "$opt" in
        c)
             CLEAN=true;;
        p)
             PACKAGE=true;;
        *)
             break;;
    esac
done

if [ "$CLEAN" = "true" ]; then
    echo "Making clean..."
    make clean
    echo "Removing build log..."
    rm -rf build.log
    echo "Cleaning package dir..."
    rm -rf out
else
    make cyanogen_d2_defconfig;

    time logsave build.log make -j4;

    if [ "$PACKAGE" = "true" ]; then
        echo " "
        if [ -e arch/arm/boot/zImage ]; then
            echo "Copying packaging components..."
            mkdir -p out
            cp -R package/* out/
            cp arch/arm/boot/zImage out/kernel/
            echo "Packaging..."
            cd out
            cdate=`date "+%Y-%m-%d"`
            zfile=Harkness-Kernel-d2-$cdate.zip
            zip -r $zfile .
            cd ..
            echo "ZIPFILE:  out/$zfile"
        else
            echo "Something went wrong. zImage not found."
        fi
    fi
fi
