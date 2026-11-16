#!/bin/bash
set -e
cd "$(dirname "$0")"
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug > /dev/null
make -j2
echo ""
./test_util