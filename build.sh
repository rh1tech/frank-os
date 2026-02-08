#!/bin/bash
# Build Rhea OS for RP2350 (M2 board only; M1 is no longer supported)
rm -rf ./build
mkdir build
cd build
cmake ..
make -j4
