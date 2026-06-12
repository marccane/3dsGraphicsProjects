#!/usr/bin/env bash
# Build the GLASS (OpenGL ES 2) RGB-triangle test into a .3dsx.
#
# Prereqs (already done once in this folder; see ../3ds-opengl-landscape.md §5):
#   1. The forked libctru is built  ->  ../ctrulib_for_GLASS/libctru
#   2. GLASS is configured+built    ->  ../GLASS/Build  (CPM fetched KYGX/RIP into ../.cpm-cache)
# This script only rebuilds the test program against those artifacts.
set -euo pipefail

export PATH="$PATH:/opt/devkitpro/devkitARM/bin:/opt/devkitpro/tools/bin"

ROOT="/home/markus/projects/1-3DS/8-opengl"
GLASS="$ROOT/GLASS"
CPM="$ROOT/.cpm-cache"
CTRU_INC="$CPM/libctru/8a80/libctru/include"          # CPM-fetched fork libctru headers (commit GLASS built against)
CTRU_LIB="$GLASS/Build/_deps/libctru-build/libctru"   # ...and its built libctru.a
KYGX_INC="$CPM/kygx/820e/Include"
RIP_INC="$CPM/rip/d67e/Include"

ARCH=(-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft)
cd "$(dirname "$0")"
mkdir -p build

echo "[1/4] picasso: compile PICA vertex shader"
picasso -o build/vshader.shbin source/vshader.v.pica

echo "[2/4] bin2s: embed shader -> object + header"
( cd build && bin2s -H vshader_shbin.h vshader.shbin > vshader_shbin.s )
arm-none-eabi-gcc -x assembler-with-cpp "${ARCH[@]}" -c build/vshader_shbin.s -o build/vshader_shbin.o

echo "[3/4] compile + link (GLASSv2 + rip + kygx + forked libctru)"
arm-none-eabi-gcc main.c build/vshader_shbin.o -o GLASS-test.elf \
  "${ARCH[@]}" -mword-relocations -ffunction-sections -fdata-sections \
  -D__3DS__ -DRIP_BACKEND=RIP_BACKEND_KYGX -std=gnu11 -O2 -Wall \
  -specs=3dsx.specs \
  -I"$GLASS/Include" -I"$KYGX_INC" -I"$RIP_INC" -I"$CTRU_INC" -Ibuild \
  -L"$GLASS/Build/Source" -L"$GLASS/Build/_deps/rip-build" -L"$GLASS/Build/_deps/kygx-build" -L"$CTRU_LIB" \
  -lGLASSv2 -lrip -lkygx -lctru -lm

echo "[4/4] 3dsxtool: package .3dsx"
3dsxtool GLASS-test.elf GLASS-test.3dsx

echo "Done -> GLASS-test.3dsx"
