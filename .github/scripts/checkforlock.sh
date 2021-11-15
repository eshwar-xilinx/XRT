#!/bin/bash

while(${{ env.LOCK_ENCLAVE }} == "true")
  do
    sleep 20m
  done
