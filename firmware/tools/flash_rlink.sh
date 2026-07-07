#!/bin/sh
# Flash a firmware ELF to the STM32 Primer2 via its on-board RLink dongle
# (Raisonance, USB 138e:9000) using OpenOCD's rlink driver (JTAG transport).
#
#   ./firmware/tools/flash_rlink.sh [firmware.elf]
#
# Default target: the LED blinky. Overwrites the on-board CircleOS in flash
# (reversible — CircleOS can be re-flashed later).
set -e
cd "$(dirname "$0")/../.."
ELF="${1:-build-fw/pocketfm_blinky.elf}"
[ -f "$ELF" ] || { echo "not found: $ELF (build it first)"; exit 1; }

echo "== flashing $ELF via RLink =="
openocd -f interface/rlink.cfg -f target/stm32f1x.cfg \
  -c "program $ELF verify reset exit"
