#!/bin/sh
# Compile-check translated game modules without the full make (safe to run in parallel).
#   sh tools/check_module.sh "3DS Files/source/game/m_0a184.c" [more.c ...]
# Generates a private protos.h (picks up decomp/protos/*.tsv) and compiles each file with the
# real 3DS flags to an object in $TMP (or /tmp). Implicit declarations are errors.
cd "$(dirname "$0")/.." || exit 1
OUT=${TMP:-/tmp}/chk_$$
mkdir -p "$OUT" || exit 1
trap 'rm -rf "$OUT"' EXIT
python tools/gen_protos.py "$OUT/protos.h" >/dev/null || exit 1
GCC=/c/devkitPro/devkitARM/bin/arm-none-eabi-gcc
[ -x "$GCC.exe" ] || [ -x "$GCC" ] || GCC=/opt/devkitpro/devkitARM/bin/arm-none-eabi-gcc
rc=0
for f in "$@"; do
    "$GCC" -c -o "$OUT/$(basename "$f" .c).o" -std=gnu11 -O2 -Wall -Wno-unused-variable \
        -Wno-unused-but-set-variable -Wno-unused-function -fno-strict-aliasing \
        -Werror=implicit-function-declaration -Werror=incompatible-pointer-types \
        -Werror=int-conversion -Werror=return-type \
        -I"$OUT" -I"3DS Files/include" -I/c/devkitPro/libctru/include -D__3DS__ \
        -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft "$f" || rc=1
done
exit $rc
