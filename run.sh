#!/bin/sh
# Builds and runs the Voyager 2 simulation. Needs a C++20 compiler and nothing else.
#   ./run.sh            then load the .glb files in out/ into a glTF viewer
#   CXX=clang++ ./run.sh
set -e
cd "$(dirname "$0")"
mkdir -p out
${CXX:-c++} -std=c++20 -O2 -Wall -Wextra -Iorbit voyager2.cpp -o out/voyager2
./out/voyager2 data out
cp models/voyager-probe-a.glb out/
