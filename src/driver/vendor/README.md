# Vendored PMD driver (from 98fmplayer)

These files are taken verbatim from https://github.com/myon98/98fmplayer
(BSD 2-Clause, (c) 2016 Takamichi Horikawa — see LICENSE here).

The PMD `.M` sequencer is bit-exact and decoupled from the sound-chip
implementation: it only touches the chip through the `struct fmdriver_work`
function pointers (`opna_writereg`/`opna_readreg`/`opna_status`). pocket-fmdsp
wires those callbacks to its own optimized OPNA emulator (`src/opna/`), so this
driver runs unmodified on top of our chip.

`ppz8.c` (8-channel PCM) is included only because the work struct references it;
it is not used by the OPNA-only Touhou PC-98 songs and is a stub candidate for
the embedded target.
