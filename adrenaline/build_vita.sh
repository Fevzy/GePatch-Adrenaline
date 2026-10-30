#!/usr/bin/env bash
# Vita side of Adrenaline (adrenaline_user.suprx) with C:\vitasdk, from Git Bash.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
export VITASDK=C:/vitasdk PATH="$PWD/tools:/c/vitasdk/bin:$PATH"
S=src B=src/build
[ -f $B/build.ninja ] || cmake -S $S -B $B -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-std=gnu17
cmake --build $B --target libvita2dfbo vitashadercollection >/dev/null 2>&1 || true
# libvita2d's own Makefile needs make; its objects are built here (BUILD_COMMAND "" in user/CMakeLists.txt)
L=$B/user/libvita2dfbo/libvita2d
if [ ! -f $L/libvita2d.a ]; then
  O=""
  for f in vita2d vita2d_texture vita2d_draw utils vita2d_image_png vita2d_image_jpeg vita2d_image_bmp vita2d_font vita2d_pgf bin_packing_2d texture_atlas int_htab; do
    arm-vita-eabi-gcc -Wl,-q -Wall -O3 -std=gnu17 -I$L/include -IC:/vitasdk/arm-vita-eabi/include/freetype2 -ffat-lto-objects -flto -c $L/source/$f.c -o $L/source/$f.o
    O="$O $L/source/$f.o"
  done
  arm-vita-eabi-ar -rc $L/libvita2d.a $L/shader/compiled/*.o $O
fi
cmake --build $B --target adrenaline_user.suprx-self 2>&1 | tee build_vita.log
ls -l $B/user/adrenaline_user.suprx
sha256sum $B/user/adrenaline_user.suprx
