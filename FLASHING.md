# Flashing the STM32 Primer2

Status of getting pocket-fmdsp firmware onto the real board.

## The problem: OpenOCD can't drive the on-board RLink

The Primer2 has a built-in **RLink** programmer (USB `138e:9000`, Jungo/Raisonance).
On Linux the only RLink driver is OpenOCD's `rlink` — and it is **JTAG-only**.

But the Primer2's target is debugged over **SWD only**: from the schematic
(`docs/schema_stm32_primer2_1_2.pdf`), the JTAG *data* pins are repurposed —
**PA15 (JTDI) → audio I²S3_WS, PB3 (JTDO) → I²S3_CK** — so only
**PA13/PA14 (SWDIO/SWCLK) + NRST** reach the RLink.

Result, reproducible:
```
$ openocd -f interface/rlink.cfg -f target/stm32f1x.cfg -c "init"
Info : only one transport option; autoselect 'jtag'
Error: JTAG scan chain interrogation failed: all zeroes   # no JTAG TAP wired
```
OpenOCD refuses `transport select swd` for rlink. So **OpenOCD + this RLink
cannot flash the Primer2.** (The RLink hardware *can* do SWD — that's how
Raisonance's own tools program it — OpenOCD just doesn't implement RLink-SWD.)

## The approach that works: Windows VM + Raisonance tools

The RLink is a **USB 1.1** device, so VirtualBox's built-in OHCI passes it
through **without** the extension pack. A minimal Windows VM runs the Raisonance
tools (which speak RLink-SWD) and flashes over the passed-through dongle.

### What's already set up (host: this machine)

- VirtualBox 7.2 VM **`pfm-primer2-flash`** (in `~/pfm-vm/`):
  - Windows (Tiny10 23H2) installed unattended; auto-logon user **`flash`** / pw **`flash`**.
  - USB filter for the RLink (`138e:9000`) → auto-attaches to the VM.
  - The firmware is on a mounted data CD: **`pocketfm_blinky.hex/.bin/.elf`**
    plus `FLASH_GUIDE.txt`.
- Firmware images are also in `build-fw/` in this repo.

### Two steps only you can do

1. **Grant VirtualBox USB access** (needs root, then a re-login):
   ```sh
   sudo usermod -aG vboxusers $USER
   # log out and back in (or reboot), then start the VM
   VBoxManage startvm pfm-primer2-flash --type gui
   ```
   Without this, the VM can't grab the RLink (`LIBUSB_ERROR_ACCESS`).

2. **Install the Raisonance tools inside the VM** (free account needed to download):
   - Get **RKit-ARM** from <https://support.raisonance.com/content/rkit-arm>
     (installs Ride7 + RFlasher7 + the RLink USB driver).
   - Then follow `FLASH_GUIDE.txt` on the data CD:
     RFlasher7 → device `STM32F103VE`, interface `RLink`/`SWD` → load
     `pocketfm_blinky.hex` → Erase, Program, Verify → reset.

### Success looks like

`pocketfm_blinky` alternately blinks **LED0 (PE0)** and **LED1 (PE1)** at ~2 Hz.
That proves build → flash → the Cortex-M3 runs our code. Then we move on to the
72 MHz clock + FSMC LCD track-list GUI (real `board_primer2.c`).

> Flashing replaces the stock **CircleOS** in flash — reversible (CircleOS can be
> re-flashed the same way).

## Alternative: external ST-Link (SWD)

If you have an ST-Link/J-Link and can reach the SWDIO/SWCLK/GND/NRST pads, this
Linux box flashes directly:
```sh
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
  -c "program build-fw/pocketfm_blinky.elf verify reset exit"
```
(No VM needed — OpenOCD *does* support SWD via ST-Link.)
