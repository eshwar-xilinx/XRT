#!/bin/bash

sudo rmmod xocl && sudo rmmod xclmgmt
sudo modprobe xocl && sudo modprobe xclmgmt

sudo rmmod xocl && sudo rmmod xclmgmt
sudo modprobe xocl && sudo modprobe xclmgmt

dmesg

