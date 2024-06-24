#!/bin/bash

x=1

while [ $x -le 50 ]
do
	sudo rmmod xocl && sudo rmmod xclmgmt
	sudo modprobe xocl && sudo modprobe xclmgmt
	x=$(( $x + 1 ))
done

dmesg

