#!/bin/bash
# Bit-exact validation oracle for ARM-specific (assembly) render optimizations.
# Renders a song on QEMU (ARM sim, whatever's compiled — incl. inline asm) and
# compares the PCM to the host x86 reference (always plain C). Identical common
# prefix => the ARM/asm path is bit-exact. Sizes differ by a few frames only
# because the sim overshoots the 6s cap by one 512-frame block; we compare the
# common prefix.
set -e
cd "$(dirname "$0")/.."
SONG="${1:-songs/touhou_pc98/Th01_04fk.M}"
SECS="${2:-6}"

cmake --build build-fw --target pocketfm_sim -j >/dev/null
cmake --build build       --target pfm         -j >/dev/null

printf '%s\tTEST\n' "$SONG" > sim_tracklist.txt
printf 'C' > sim_input.txt
rm -f sim_play_*.wav
timeout 120 qemu-system-arm -M netduino2 -cpu cortex-m3 -nographic \
  -semihosting-config enable=on,target=native -kernel build-fw/pocketfm_sim.elf 2>/dev/null || true
SIM=$(ls sim_play_*.wav 2>/dev/null | head -1)
[ -n "$SIM" ] || { echo "FAIL: sim produced no wav"; exit 1; }
./build/pfm render "$SONG" /tmp/_hostref.wav "$SECS" >/dev/null

tail -c +45 "$SIM" > /tmp/_sim.pcm
tail -c +45 /tmp/_hostref.wav > /tmp/_host.pcm
N=$(( $(stat -c%s /tmp/_host.pcm) < $(stat -c%s /tmp/_sim.pcm) ? $(stat -c%s /tmp/_host.pcm) : $(stat -c%s /tmp/_sim.pcm) ))
rm -f sim_tracklist.txt sim_input.txt "$SIM"
if cmp -s <(head -c "$N" /tmp/_sim.pcm) <(head -c "$N" /tmp/_host.pcm); then
  echo "BIT-EXACT ✓  ($SONG, ${N} PCM bytes match)"
else
  echo "MISMATCH ✗  ($SONG) — ARM path diverges from host C reference"
  exit 1
fi
