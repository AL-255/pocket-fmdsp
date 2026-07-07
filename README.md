# pocket-fmdsp

A small, fast, **integer-only** player for PC-98 **PMD** (`.M`) music, targeting an
**STM32F103V** (ARM Cortex-M3, no FPU/SIMD/DSP, 512 kB Flash / 64 kB RAM),
streaming songs from an SD card.

The OPNA (YM2608) sound chip is **reimplemented and optimized** for the M3; the PMD
sequence driver is **ported** from the reference for bit-exact `.M` parsing.
Correctness reference: <https://github.com/myon98/98fmplayer>.
See [DESIGN.md](DESIGN.md) for the full architecture, memory budget, and survey.

## Status

| Component | State |
|-----------|-------|
| OPNA FM (6ch × 4op, 8 algorithms, ADSR, feedback, CH3 mode) | ✅ done, **bit-exact** |
| OPNA SSG (3 tone + noise + envelope, box resampler) | ✅ done, **bit-exact** |
| OPNA rhythm/drum (on-the-fly 8 kB ROM decode) | ✅ done (needs a ROM to sound) |
| OPNA ADPCM-B | ⛔ stubbed (256 kB sample RAM — later) |
| Timer A/B + render loop | ✅ done, bit-exact |
| PMD `.M` loader (header, pointer table, instruments) | ✅ done, verified |
| PMD sequencer (tick engine, ~120 opcodes) | ✅ done (vendored, drives our OPNA) |
| Full song playback → WAV | ✅ **plays all 108 test songs** |
| Cortex-M3 cross-compile | ✅ ~34 kB flash (`scripts/check-m3.sh`) |
| **STM32 Primer2 firmware + QEMU env** (LCD track list, joystick, Shift-JIS) | ✅ runs in QEMU — see [`firmware/`](firmware/README.md) |
| Real Primer2 peripheral drivers (FSMC LCD, SDIO, I2S codec) | 🚧 skeleton in `firmware/bsp/primer2` |

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
Pass an 8 kB YM2608 rhythm ROM as the 5th `render` arg to make drums audible.

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

1. Cortex-M3 firmware: FatFs/SDIO to stream `.M` from SD, I2S/DAC DMA double-buffer
   at the audio rate, run under QEMU/Renode. Stub `ppz8` (unused by these songs) to
   reclaim ~5.8 kB flash + ~7 kB RAM.
2. Output resampling 55466 → 44100/48000 Hz for real codecs (cheap linear/poly).
3. Optional: ADPCM-B streamed from SD (small window instead of 256 kB RAM); drum ROM
   on SD.
