#!/bin/bash

CWD=$(readlink -f .)
RETURN_CODE=0
DISTRO=`grep '^ID=' /etc/os-release | awk -F= '{print $2}' | tr -d '"'`

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
                        RETURN_CODE=1
                else
                        echo "Drivers are loaded"
                fi
        else
                echo "Package XRT is NOT installed!"
		RETURN_CODE=1
        fi
else
	dpkg -s xrt  &> /dev/null
	if [ $? -eq 0 ]; then
		echo "Package XRT is installed!"
		DRIVER=`lsmod | grep -i xocl`
		if [ -z "$DRIVER" ]; then
			echo "Drivers are NOT loaded"
		 	RETURN_CODE=1
		else
      echo "Drivers are loaded"
    		fi
	else
		echo "Package XRT is NOT installed!"
		RETURN_CODE=1
	fi
fi

cat `find /var/lib/dkms/xrt/ -iname "make.log"`
exit $RETURN_CODE

