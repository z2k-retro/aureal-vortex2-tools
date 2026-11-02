#!/usr/bin/env bash
# Build a3dtest.exe for Windows 98 with the MinGW cross compiler, without a C runtime.
# Needs the A3D 2.0 SDK header: set SDK_INC to the folder holding ia3dapi.h
#   (git clone https://github.com/sigmaco/a3d-sdk; SDK_INC=a3d-sdk/a3d-sdk-v2.0/sdk/inc)
set -e
SDK_INC=${SDK_INC:-./a3d-sdk/a3d-sdk-v2.0/sdk/inc}
CC=${CC:-i686-w64-mingw32-gcc}
$CC -std=gnu99 -O2 -march=i586 -mfpmath=387 -ffreestanding -fno-builtin -fno-stack-protector \
    -fno-asynchronous-unwind-tables -Wall -Wno-unused-function -I"$SDK_INC" \
    -nostdlib -Wl,--entry,_entry -Wl,--subsystem,windows:4.0 \
    -Wl,--major-os-version,4 -Wl,--minor-os-version,0 \
    -Wl,--major-subsystem-version,4 -Wl,--minor-subsystem-version,0 \
    -Wl,--disable-dynamicbase -Wl,--disable-nxcompat -Wl,--disable-reloc-section \
    -o a3dtest.exe a3dtest.c -lkernel32 -luser32 -lole32 -luuid -lwinmm -lgcc
echo built a3dtest.exe
