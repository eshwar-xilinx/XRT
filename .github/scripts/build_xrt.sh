#!/bin/bash

PR_ID=$1
CWD=$(readlink -f .)
DISTRO=$(grep '^ID=' /etc/os-release | awk -F= '{print $2}' | tr -d '"')

sudo dmesg -C

sudo $CWD/XRT/src/runtime_src/tools/scripts/xrtdeps.sh 

if [[ $DISTRO == "centos" ]]; then
        source /opt/rh/devtoolset-*/enable
fi

export XILINX_VITIS=/mnt/packages/vitis
export PATH=/mnt/packages/vitis/gnu/microblaze/lin/bin:/mnt/packages/vitis/gnu/microblaze/linux_toolchain/lin64_le/bin:$PATH

$CWD/XRT/build/build.sh -opt
