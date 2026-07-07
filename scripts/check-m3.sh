#!/bin/sh
# Cross-compile the freestanding core + driver for ARM Cortex-M3 (the STM32F103V
# core) and report code size. Compile-only smoke test — no board/linker script yet
# (the target is "the CPU core in a simulator"). Proves the code is embeddable.
#
# Requires: arm-none-eabi-gcc, arm-none-eabi-size
set -e
cd "$(dirname "$0")/.."

CC=${CC:-arm-none-eabi-gcc}
SIZE=${SIZE:-arm-none-eabi-size}
CF="-mcpu=cortex-m3 -mthumb -Os -ffunction-sections -fdata-sections \
    -Iinclude -Isrc/opna -Isrc/driver/vendor"
OUT=build/m3
mkdir -p "$OUT"

# our optimized core (strict warnings)
for f in src/opna/opna.c src/opna/opna_fm.c src/opna/opna_ssg.c \
         src/opna/opna_drum.c src/opna/opna_timer.c \
         src/driver/pmd_load.c src/driver/player.c; do
  $CC $CF -Wall -Wextra -c "$f" -o "$OUT/$(basename "$f" .c).o"
done
# vendored PMD driver (its own warnings suppressed)
for f in fmdriver_pmd fmdriver_common ppz8; do
  $CC $CF -w -c "src/driver/vendor/fmdriver/$f.c" -o "$OUT/$f.o"
done

echo "Cortex-M3 (Thumb2, -Os) object sizes:"
$SIZE -t "$OUT"/*.o
