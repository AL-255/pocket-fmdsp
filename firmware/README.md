# pocket-fmdsp firmware (STM32 Primer2) + QEMU environment

Bare-metal firmware for the **STM32 Primer2** (STM32F103VET6, Cortex-M3, 512 kB
Flash / 64 kB RAM) that lists the PMD `.M` tracks on the LCD and plays the one you
pick with the joystick — running the same bit-exact OPNA player as the host build.

A **QEMU emulation environment** runs the identical firmware on an emulated
Cortex-M3 so the GUI + audio pipeline can be developed and verified before touching
hardware.

## Modern gcc-arm-none-eabi build flow

No vendor SPL / StdPeriph / CircleOS. Just:
- `cmake/arm-none-eabi.cmake` — CMake toolchain file
- `ld/*.ld` — linker scripts (Primer2 64 kB, or QEMU/netduino2 128 kB)
- `startup/startup.c` — Cortex-M3 vector table + `.data`/`.bss` init
- CMSIS-free minimal register access; newlib (`nosys.specs`) for `memcpy`/`memset`

## Board HAL

`bsp/board.h` abstracts the hardware; two backends implement it:

| | `bsp/sim` (QEMU) | `bsp/primer2` (real HW) |
|---|---|---|
| LCD | RGB565 framebuffer → PPM screenshots | FSMC 8080 → 128×160 panel GRAM |
| Joystick | scripted input file | GPIO PE2-5 / PA8 |
| Audio | streamed to a host WAV | STW5094A codec via I2S2 + DMA |
| Storage | host files via a manifest | microSD via SDIO + FatFs |

I/O in the sim uses **ARM semihosting**, so nothing but the CPU core needs to be
emulated. The real-hardware pin map (from `docs/schema_stm32_primer2_1_2.pdf`) is
in `bsp/primer2/primer2_pins.h`; `board_primer2.c` is the deployment skeleton.

## Shift-JIS font

Song metadata and UI labels are Japanese. `gui/font_sjis.h` is generated from the
public-domain **Misaki gothic 8×8** BDF (`third_party/misaki/`) by
`tools/genfont_sjis.py` — half-width (4×8) ASCII/kana + full-width (8×8) JIS X 0208,
indexed by the packed `lead*188+trail` SJIS code for O(1) lookup. `gui/gfx.c`
decodes SJIS and renders half/full-width glyphs at integer scale.

## Build & run in QEMU

```sh
# 1) build the sim firmware for Cortex-M3
cmake -S firmware -B build-fw \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/firmware/cmake/arm-none-eabi.cmake
cmake --build build-fw

# 2) run under QEMU (netduino2 = STM32F205 Cortex-M3), joystick script "DDD C R C"
#    = down x3, play, page-down, play. Produces screenshots + audio in build-fw/sim-run/.
./firmware/tools/run_qemu_sim.sh "DDD C R C"
```

Outputs in `build-fw/sim-run/`:
- `sim_shot_NNN.ppm` — LCD screenshots (128×160), plus `contact_sheet.png`
- `sim_play_NNN.wav` — audio of the selected song (bit-identical to `pfm render`)

To regenerate the fonts/strings after editing:
```sh
python3 firmware/tools/genfont_sjis.py    # gui/font_sjis.h
python3 firmware/tools/gen_ui_strings.py  # gui/ui_strings.h
```

## Verified

- Boots on QEMU `netduino2` (Cortex-M3), semihosting up, `.data`/`.bss` correct.
- GUI lists all 108 test tracks, joystick navigation + paging, now-playing screen.
- Shift-JIS renders (title 東方 PC-98 FM, game names 東方靈異伝 …, status 再生中/完了).
- Audio rendered on the emulated core → WAV is **byte-for-byte identical** to the
  host `pfm render` output.
- Flash 107.6 kB (72 kB font + 35 kB code); sim RAM 111 kB / 128 kB (incl. 40 kB
  framebuffer). Real Primer2 build streams to GRAM (no framebuffer) → fits 64 kB.

## Deploying to real hardware (next)

1. Flesh out `bsp/primer2/board_primer2.c`: clock tree (12 MHz→72 MHz), FSMC LCD
   init (ST7735-class), GPIO joystick, SDIO+FatFs, I2S2+DMA to the codec.
2. Link with `ld/stm32f103ve.ld` (64 kB); drop the sim framebuffer (draw to GRAM).
3. Flash via the on-board RLink/ST-Link (SWD).
