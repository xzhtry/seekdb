#!/bin/sh
set -eu
CXX="${CXX:-g++}"
mkdir -p build
$CXX -O2 -std=c++17 -Wall -Wextra -Wno-unused-parameter \
  -static-libstdc++ -static-libgcc \
  src/main.cpp src/engine.cpp src/sql.cpp src/util.cpp \
  -o build/minivectordb
