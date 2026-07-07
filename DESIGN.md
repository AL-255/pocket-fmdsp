# pocket-fmdsp — PC-98 OPNA FM player for STM32F103V

A small, fast, integer-only player for PC-98 **PMD** (`.M`) music, targeting an
**STM32F103V** (ARM Cortex-M3, 72 MHz, **no FPU / no SIMD / no DSP ext**,
512 kB Flash, 64 kB RAM), streaming songs from an SD card.

Correctness reference: <https://github.com/myon98/98fmplayer> (`libopna` + `fmdriver`).
We *reimplement* the OPNA emulator (optimized for M3) and *port* the PMD sequencer
(its byte-level command semantics must match the reference bit-for-bit or `.M`
files desync).

---

## 1. The chip: YM2608 "OPNA"

An OPNA has four sound sub-blocks. Everything is driven by one register-write bus
(`opna_writereg(reg, val)`; `reg >= 0x100` selects the A1=1 bank for FM ch4-6 &
ADPCM) and mixed into one interleaved-stereo `int16` stream.

| Block | Voices | Role in PMD | Our plan |
|-------|--------|-------------|----------|
| **FM** | 6 ch × 4 op | Melody/bass/pads | Reimplement (hot path) |
| **SSG** | 3 sq + noise | Melody/arps/SE | Reimplement (hot path) |
| **Rhythm (drum)** | 6 PCM (ROM) | Drums | On-the-fly ADPCM decode from 8 kB ROM |
| **ADPCM-B** | 1 PCM (256 kB RAM) | Optional PCM | **Stubbed** initially (RAM-prohibitive) |

**Native mix rate = master/144 = 7 987 200 / 144 = 55 466.6 Hz**, interleaved
signed 16-bit stereo. (A real DAC at 44.1/48 kHz needs a resample step, TODO.)

### FM synthesis (per output sample)
Classic Yamaha log-domain pipeline, fully integer, 2 table lookups per operator:
```
pind   = (phase>>10) + (modulation>>1)          // quarter-wave fold via top 2 bits
logout = logsintable[pind&255] + (env<<2) + (tl<<5)
out    = (exptable[logout&255] << 2) >> (logout>>8)   // shifter clamped <= 13
```
- **Phase**: 20-bit accumulator; `freq=(fnum<<blk)>>1`, `+det`, `phase += (freq*mul)>>1`.
- **Envelope (ADSR)**: runs **once per 3 samples** (`env_div3`); rates precomputed
  into `{selector, mul, shifter}` on any param/keycode change; `rateinctable` gives
  the per-tick increment. Attack is multiplicative, D/S/R additive.
- **8 algorithms + feedback** routed in `opna_fm_chanout`; algorithms 4-7 use a
  1-sample-delayed accumulator (`alg_mem`) for the hardware's L/R phase-difference.
- We **drop the optional hi-res sin/env** paths (disabled upstream by default) →
  saves the 2 kB `logsintable_hires` and a branch per operator.

### SSG synthesis + resampling
- Raw generation at master/32 = 249 600 Hz: 3 tone dividers, 17-bit noise LFSR,
  5-bit envelope, `voltable[32]`.
- Resample 249 600 → 55 466 (ratio 9/2, index counts in half-samples, +9/output).
- Reference default is **YMF288 mode = a 5-tap box average /9** (cheap). The
  "OPNA analog" mode uses a **128-tap × 3ch sinc** (≈21 M MAC/s) — we **drop it**.
- **Optimization:** the reference does a **1 kB `memcpy` per output sample**
  (~55 MB/s @ 72 MHz just to make the sinc window contiguous). The box-average only
  reads a 6-sample window, so we use a small modulo-indexed ring (32×3 `int16` ≈
  192 B) and **eliminate the memcpy and the 2 kB resampler buffer entirely**.

### Rhythm/drum
- Reference decodes the **8 kB ADPCM-A ROM → ~105 kB of `int16` PCM held in RAM**
  (each nibble upsampled ×3 or ×6). **Impossible in 64 kB.**
- **Optimization:** keep the raw 8 kB ROM in flash/SD and **decode on-the-fly** per
  voice (`acc`, `step`, nibble addr, ×div hold counter → one nibble every div
  output samples). Per-voice state ~24 B × 6 ≈ 150 B RAM; identical output.
- Drums are **optional**: if no ROM is provided, drum voices are silent.

### ADPCM-B
- Needs 256 kB sample DRAM → dropped for the first cut. The PMD driver treats
  ADPCM opcodes as byte-skippers so the stream stays aligned. (Later: stream the
  referenced bytes from SD into a small window.)

---

## 2. The format: PMD `.M`

`pmd_load(data, len)` — `data` = whole file; `base = data+1`; `datalen = len-1`.
- Reject `data[0] (=OPM/version flag) > 1` or `len < 0x19`.
- **Pointer table** at `base` (little-endian `uint16`, offsets into `base`):
  `base+0..0x15` = 11 part pointers (FM1-6, SSG1-3, ADPCM, RHYTHM);
  `base+0x16` = rhythm pattern-table base (`r_offset`);
  `base+0x18` = FM instrument table base (`tone_ptr`) *unless* `base[0]==0x18`
  (then no instrument table).
- **FM instrument** = 26-byte record: `tonenum` byte + 25 params. Linear-scan from
  `tone_ptr` in 0x1a strides matching `tonenum`. Params → YM2608 regs per slot
  s=0..3 (physical order S1,S3,S2,S4): `p[0x00+s]→0x30`(DT/ML) `p[0x04+s]→0x40`(TL)
  `p[0x08+s]→0x50`(KS/AR) `p[0x0c+s]→0x60`(AM/DR) `p[0x10+s]→0x70`(SR)
  `p[0x14+s]→0x80`(SL/RR); `p[0x18]→0xB0`(FB/ALG).
- **Sequence bytes**: `< 0x80` = note (low nibble semitone 0-11, `0x_F`=rest, high
  nibble octave) followed by a length byte (96 ticks/whole note); `>= 0x80` =
  command (`0x80`=end/loop). ~120 opcodes across 5 dispatch tables; many are
  fixed-width arg skippers.
- The whole file stays **resident** (parts seek backward via `loop_ptr`/`tone_ptr`/
  `r_ptr`; `ptr` is a `uint16` offset). Largest test song 21 kB; typical 2-8 kB.

---

## 3. The engine: tick-driven sequencer

- One **Timer-B overflow = one music tick**; the driver re-writes reg 0x26 each tick
  to re-arm. Timer-B period in output samples = `16*(256 - timerb)`.
- **Tempo ↔ timerb**: `tempo = 0x112C / (0x100 - timerb)` (with rounding); reverse
  likewise. Cortex-M3 hardware `UDIV` makes the handful of divides free.
- Timer-A is a faster sub-tick for LFO fine-timing / fadeout / SSG-effects.
- Per tick: walk parts, `len_cnt--`, key-off at `len_cnt <= gate`, apply software
  LFO / portamento / detune / SSG-envelope, decode the next note/command when
  `len_cnt==0`, emit reg writes (fnum/blk, per-slot TL, key-on 0x28, pan 0xB4).
- **No float anywhere** in the tick path — the reference is already fixed-point.
- **Port strategy:** reproduce `pmd_proc_parts` + the FM/SSG per-part state machines
  + command tables faithfully; **stub** ADPCM/PPZ8/PPS/SSG-eff handlers as
  byte-skippers; drop FMP driver, PPZ8, oscillo/leveldata/FMDSP UI state.

---

## 4. Driver ↔ chip boundary

`struct fmdriver_work` (function-pointer interface, from the reference, trimmed):
```
driver_opna_interrupt(work)          // sequencer tick, fired on Timer-B/A overflow
opna_writereg(work, addr, data)      // addr>=0x100 => A1=1 bank
opna_readreg(work, addr)
opna_status(work, a1)
```
Audio callback core (`opna_timer_mix`): `memset(buf,0)`, then a do/while loop that
slices the request at each timer-overflow boundary, calls `opna_mix` for the slice,
and fires `driver_opna_interrupt` on Timer-B wrap. Output: interleaved stereo
`int16` @ 55466 Hz.

---

## 5. Memory budget (STM32F103V: 512 kB Flash / 64 kB RAM)

### Flash (const)
| Item | Size |
|------|------|
| FM `logsintable`+`exptable`+`dettable`+`rateinctable` | ~1.2 kB |
| SSG `voltable` | 64 B |
| drum `steps`+`step_inc` | ~106 B |
| drum ROM (optional, or from SD) | 8 kB |
| Code (opna + driver + platform) | ~40-60 kB |
| **Total** | **well under 128 kB** |

### RAM
| Item | Size |
|------|------|
| `struct opna` (FM ~700 B + SSG ~80 B + ring ~200 B + drum ~150 B) | ~1.2 kB |
| `struct driver_pmd` (trimmed: FM6+FM3ex + SSG3, no ADPCM tbl/PPZ8) | ~2-3 kB |
| `struct fmdriver_work` (trimmed) | ~0.5 kB |
| Resident song buffer | ≤ ~24 kB (cap) |
| Audio double-buffer (2×512 frames stereo) | ~4 kB |
| Stack / misc | ~4 kB |
| **Total** | **≈ 12-36 kB** (fits 64 kB) |

---

## 6. Build targets

- **Host simulator (now):** portable C, renders `.M` → WAV @ 55466 Hz. Also a
  register-level self-test (program FM/SSG directly, no driver) to validate the
  synth in isolation. This is the "CPU core in simulators" target for iteration.
- **Cortex-M3 (later):** `arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb -Os`, FatFs +
  SDIO for the SD card, I2S/DAC + DMA double-buffer at the audio rate, run under
  QEMU/Renode. No board specifics yet — the core is a freestanding library plus a
  thin platform HAL.

### Layout
```
include/pfm/   public headers (opna*, fmdriver, pmd, config)
src/opna/      OPNA emulator (reimplemented, optimized)
src/driver/    PMD loader + sequencer (ported)
src/platform/  file/audio HAL (host + SD backends)
host/          WAV writer + CLI (selftest / render)
```

## 7. Verification plan

1. **Synth in isolation** — self-test programs a known FM patch + SSG tone; assert
   the WAV is non-silent with the expected fundamental (FFT peak). ✅ first.
2. **Register-trace oracle** — build the reference `libopna`+`fmdriver` on host, dump
   its `opna_writereg` trace for a song, and diff against our port's trace to catch
   sequencer divergence deterministically. (planned)
3. **Ear/spectrum** — render Touhou `.M` files and compare against the reference WAV.
