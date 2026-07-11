# pocket-fmdsp

pocket-fmdsp is a from-scratch, **integer-only** player for PC-98 **PMD** (`.M`)
chiptune, running on an **STM32 Primer2** (ARM Cortex-M3 @ 132 MHz, 512 kB flash /
64 kB RAM — no FPU, SIMD, or DSP). It reimplements and hand-optimizes the **OPNA
(YM2608)** FM + SSG + rhythm synthesis for that core, streaming songs from a microSD
card through a bit-bang SD driver, FreeRTOS, FatFs, and an I²S codec. On-device it
has a hierarchical file browser, a tabbed Browser/Playback/Settings UI, per-voice
muting, digital volume, and a live per-task CPU meter. FM/SSG are bit-exact and
assembly-optimized; drums play PCM samples embedded in flash. The PMD sequencer is
ported from the reference for bit-exact `.M` parsing.

Correctness reference: <https://github.com/myon98/98fmplayer>.
See [DESIGN.md](DESIGN.md) for the full architecture, memory budget, and survey.

## Status

| Component | State |
|-----------|-------|
| OPNA FM (6ch × 4op, 8 algorithms, ADSR, feedback, CH3 mode) | ✅ done, **bit-exact** |
| OPNA SSG (3 tone + noise + envelope, box resampler) | ✅ done, **bit-exact** |
| OPNA rhythm/drum (flash-embedded PCM samples) | ✅ done, audible on device |
| OPNA ADPCM-B | ⛔ stubbed (256 kB sample RAM — later) |
| Timer A/B + render loop | ✅ done, bit-exact |
| PMD `.M` loader (header, pointer table, instruments) | ✅ done, verified |
| PMD sequencer (tick engine, ~120 opcodes) | ✅ done (vendored, drives our OPNA) |
| Full song playback → WAV | ✅ **plays all 108 test songs** |
| Cortex-M3 cross-compile | ✅ ~34 kB flash (`scripts/check-m3.sh`) |
| **STM32 Primer2 firmware** (SD browser, tab UI, joystick, Shift-JIS) | ✅ runs on hardware + QEMU — see [`firmware/`](firmware/README.md) |
| Real Primer2 peripheral drivers (FSMC LCD, bit-bang SD-SPI, STW5094A I²S codec) | ✅ done, on hardware |
| FreeRTOS (codec > SD > OPNA render > LCD priority) + assembly SSG/SD-SPI | ✅ done |

## What works now

The player is **complete and validated on the host simulator**: it loads any PMD
`.M`, runs the sequencer on our optimized OPNA, and renders audio.

**Verified bit-exact against the reference.** An oracle (reference driver +
reference `libopna`) renders ground-truth WAVs; our player's output is compared
sample-for-sample:

- `Th01_01.M`, 8 s (443 728 frames): **max sample diff = 0**, correlation
  `1.00000000`.
- 15 songs spanning TH1–TH5 + the largest song: **all bit-identical** (maxdiff 0).

So our reimplemented, optimized OPNA reproduces the reference **exactly** while
using **~1.9 kB RAM** (`struct opna`) instead of the reference's ~105 kB drum PCM +
2 kB SSG resampler, and eliminating a 1 kB-per-sample memcpy.

**Measured Cortex-M3 footprint** (Thumb2, `-Os`): OPNA core ~5.3 kB code
(FM 3.2 kB / SSG 730 B / drum 728 B), PMD driver+glue ~23 kB, ppz8 5.8 kB (stub
candidate) → **~34 kB flash** total. `struct opna` 1.9 kB + driver ~4 kB + work
~2 kB + song buffer (≤21 kB) fit 64 kB RAM easily.

The self-test also validates the synth in isolation (FM peak 4084 = exact
reference ceiling; 440 Hz tones with clean spectral peaks).

## Build & run (host simulator)

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Play a full song to WAV (8 s):
./build/pfm render "songs/touhou_pc98/TH1 - HRtP/Th01_01.M" out.wav 8

# Validate the synth in isolation (FM/SSG test tones + checks):
./build/pfm selftest build/out

# Inspect a PMD .M file:
./build/pfm info "songs/touhou_pc98/TH1 - HRtP/Th01_01.M"

# Cross-compile the core+driver for Cortex-M3 and report size:
./scripts/check-m3.sh
```

Output is interleaved-stereo signed-16-bit WAV at 55466 Hz (the OPNA native rate).
Drums play the flash-embedded PCM samples automatically — no ROM needed.

## Build the firmware (STM32 Primer2)

Needs the `gcc-arm-none-eabi` toolchain on `PATH`.

```sh
# configure once with the bare-metal ARM toolchain file
cmake -S firmware -B build-fw \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/firmware/cmake/arm-none-eabi.cmake
cmake --build build-fw -j

# targets:
#   build-fw/pocketfm_hw.elf   real Primer2 firmware (SD browser + player)
#   build-fw/pocketfm_sim.elf  QEMU/semihosting build (same player, host I/O)
```

Run the sim build under QEMU (no hardware needed) — see
[`firmware/README.md`](firmware/README.md):

```sh
./firmware/tools/run_qemu_sim.sh "DDD C R C"   # scripted joystick input
```

Flashing `pocketfm_hw.elf`/`.hex` to real hardware uses an ST-LINK/RLink SWD probe;
see [FLASHING.md](FLASHING.md). Prebuilt ELFs are attached to each
[GitHub release](../../releases).

Every optimization to the OPNA is validated **bit-exact** against the host C
reference by re-rendering on the QEMU ARM sim and diffing PCM
(`scripts/validate-arm-audio.sh <song.M>`).

## Layout

```
include/pfm/        public headers (opna*, pmd, player, config)
src/opna/           OPNA emulator (reimplemented, optimized) — the hot path
src/driver/         PMD loader (ours) + player glue
src/driver/vendor/  PMD sequencer, vendored from 98fmplayer (BSD-2), drives our OPNA
host/               WAV writer + CLI (selftest / info / render)
firmware/           STM32 Primer2 bare-metal firmware + QEMU env (GUI, HAL, fonts)
third_party/misaki/ Misaki 8x8 Shift-JIS bitmap font (public domain)
scripts/            check-m3.sh (Cortex-M3 cross-compile smoke test)
reference/oracle/   ground-truth harness (reference driver + reference OPNA)
```

The performance-critical OPNA (the 55 kHz sample loop) is **our** optimized
reimplementation; the PMD sequencer (tick-rate, ~hundreds of Hz — not hot) is the
reference's bit-exact driver, wired to our chip through its function-pointer
interface. This gives correct playback with the speed/size wins where they matter.

## Next

1. Further OPNA speedups: the FM operator core is the remaining hot path (~70% of
   render); explore bit-exact wins beyond the envelope OFF-slot skip already landed.
2. ADPCM-B (`ppz8`) streamed from SD in a small window instead of 256 kB RAM.
3. Output resampling 55466 → 44100/48000 Hz for other codecs (cheap linear/poly).
