#!/bin/sh
# Build "EMB Thumbnails Setup.exe" (64-bit Windows) on Linux with MinGW-w64:
#   sudo apt install g++-mingw-w64-x86-64
#   ./build.sh
set -e
cd "$(dirname "$0")"
mkdir -p build
CXX=x86_64-w64-mingw32-g++
WINDRES=x86_64-w64-mingw32-windres
FLAGS="-O2 -std=c++17 -municode -DUNICODE -D_UNICODE -static -static-libgcc -static-libstdc++ -s"

$CXX $FLAGS -shared -o build/EmbThumbnails.dll EmbThumbnailProvider.cpp EmbThumbnails.def \
    -lole32 -luuid -lshlwapi -lshell32 -lwindowscodecs -lgdi32 -ladvapi32

$WINDRES setup.rc -O coff -o build/setup.res
$CXX $FLAGS -mwindows -o "build/EMB Thumbnails Setup.exe" setup.cpp build/setup.res \
    -lole32 -luuid -lshell32 -ladvapi32 -luser32

ls -l build/*.exe build/*.dll
