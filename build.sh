#!/bin/sh
set -e
cd "$(dirname "$0")"
mkdir -p out
CXX=x86_64-w64-mingw32-g++-posix
x86_64-w64-mingw32-windres res/juice.rc -O coff -o out/res.o
$CXX -std=c++17 -O2 -municode -mwindows -Wall -Wno-unknown-pragmas -Wno-missing-field-initializers \
  src/*.cpp out/res.o -o out/Juice.exe \
  -static -static-libgcc -static-libstdc++ \
  -lsetupapi -lhid -lcfgmgr32 -lruntimeobject -lole32 -lshell32 -luser32 -lgdi32 -ldwmapi -ladvapi32 -lshlwapi -lgdiplus -luuid
x86_64-w64-mingw32-strip out/Juice.exe
ls -la out/Juice.exe
