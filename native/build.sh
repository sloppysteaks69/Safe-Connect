#!/usr/bin/env bash
# Builds safe-connect-1.1.exe with MinGW-w64 (Linux or WSL: apt install g++-mingw-w64-x86-64).
set -euo pipefail
cd "$(dirname "$0")"
x86_64-w64-mingw32-windres safe_connect.rc -O coff -o safe_connect.res
x86_64-w64-mingw32-g++ -std=c++17 -Os -s -municode -mwindows -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-asynchronous-unwind-tables \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -static -static-libgcc -static-libstdc++ \
    -o ../safe-connect-1.1.exe safe_connect.cpp safe_connect.res \
    -lgdiplus -lws2_32 -lgdi32 -luser32 -lshell32
rm -f safe_connect.res
ls -l ../safe-connect-1.1.exe
