#!/bin/bash

ROOT_DIR=$(pwd)

git clone -b topic/udp https://github.com/actor-framework/actor-framework.git
cd actor-framework
./configure --build-type=release --no-opencl --no-tools --no-examples
make -j4
cd $ROOT_DIR


