#!/bin/bash

CWD=$(readlink -f .)
#####################################################################################################
BDF1=$(lspci -d 10ee: | awk '{print $1}' | awk ' FNR == 1')
DEVID1=$(lspci -d 10ee: | awk '{print $7}' | awk ' FNR == 1')

BDF2=$(lspci -d 10ee: | awk '{print $1}' | awk ' FNR == 2')
DEVID2=$(lspci -d 10ee: | awk '{print $7}' | awk ' FNR == 2')

DISTRO=$(grep '^ID=' /etc/os-release | awk -F= '{print $2}' | tr -d '"')

FLAG=0

if [[ $DEVID1 == "5001" || $DEVID1 == "5005" || $DEVID1 == "5021" ]]; then
        USERPF=$BDF1
        MGMTPF=$BDF2
else
        USERPF=$BDF2
        MGMTPF=$BDF1
fi

###############################################################################
##                       checking distro 				     ##
###############################################################################

cd $CWD/XRT/build/Release
if [[ $DISTRO == "centos" ]]; then
        sudo yum install -y ./xrt_*xrt.rpm
else
        sudo apt install ./xrt_*xrt.deb
fi

if [[ $DISTRO == "centos" ]]; then
	rpm -q xrt &> /dev/null
        if [ $? -eq 0 ]; then
                echo "Package XRT is installed!"
                DRIVER=`lsmod | grep -i xocl`
                if [ -z "$DRIVER" ]; then
                        echo "Drivers are NOT loaded"
                        FLAG=1
                else
                        echo "Drivers are loaded"
                fi
        else
                echo "Package XRT is NOT installed!"
		FLAG=1
        fi
else
	dpkg -s xrt  &> /dev/null
	if [ $? -eq 0 ]; then
		echo "Package XRT is installed!"
		DRIVER=`lsmod | grep -i xocl`
		if [ -z "$DRIVER" ]; then
			echo "Drivers are NOT loaded"
		 	FLAG=1
		else
      echo "Drivers are loaded"
    		fi
	else
		echo "Package XRT is NOT installed!"
		FLAG=1
	fi
fi

###############################################################################
#  installing platform packages
###############################################################################


if [[ $DEVID1 != "5005" ]]; then	#for u250 card, shell needs to be updated
        if [[ $DISTRO == "centos" ]]; then
		cd $CWD/u*
                sudo yum install -y ./xilinx*.rpm
        else
		sudo rm -rf $CWD/ubuntu* > /dev/null 2>&1
		cd $CWD/u*
                sudo apt install ./xilinx*.deb
        fi
else
	sudo rm -rf $CWD/ubuntu* > /dev/null 2>&1
        cd $CWD/u*
        sudo apt install ./xilinx-cmc*.deb
        sudo apt install ./xilinx-sc-fw*.deb
        sudo apt install ./xilinx*base*.deb
        sudo /opt/xilinx/xrt/bin/xbmgmt --base --device $MGMT_PF
        sudo apt install ./xilinx*validate*.deb
        sudo apt install ./xilinx*shell*.deb
        sudo /opt/xilinx/xrt/bin/xbmgmt program --shell /opt/xilinx/firmware/u250/gen3x16/xdma-shell/partition.xsabin --device $MGMT_PF

fi

cat `find /var/lib/dkms/xrt/ -iname "make.log"`
if [ $FLAG == 1 ]; then
	exit 1
fi

