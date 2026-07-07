#ifndef PFM_OPNA_DRUM_H_INCLUDED
#define PFM_OPNA_DRUM_H_INCLUDED

/*
 * OPNA rhythm block: 6 PCM drum voices (BD/SD/TOP/HH/TOM/RIM) from the 8kB
 * YM2608 ADPCM-A rhythm ROM. Unlike 98fmplayer (which pre-expands the ROM to
 * ~105kB of int16 PCM in RAM), we keep the raw 8kB ROM in flash/SD and decode
 * on the fly, holding only per-voice decoder state (~24B x 6).
 *
 * The ROM is optional: if opna_drum_set_rom() is never called (rom==NULL), the
 * drum voices stay silent.
 */

#include "pfm/pfm_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PFM_DRUM_ROM_SIZE 0x2000 /* 8192 bytes */

struct opna_drum_voice {
  unsigned start;  /* first nibble index (start_byte << 1) */
  unsigned end;    /* stop when (addr>>1) == end (byte) */
  unsigned div;    /* output-sample replication per decoded nibble (3 or 6) */
  bool playing;
  unsigned addr;   /* current nibble index */
  int acc;         /* ADPCM accumulator */
  int step;        /* ADPCM step index (0..48) */
  unsigned sub;    /* 0..div-1 replication counter */
  int16_t cur;     /* current decoded sample (<<4) */
  uint8_t level;   /* 0..31 per-voice attenuation */
  bool left, right;
};

struct opna_drum {
  const uint8_t *rom;      /* 8kB ADPCM-A ROM, or NULL */
  struct opna_drum_voice v[6];
  unsigned total_level;    /* 0..63 */
  unsigned mask;           /* 1<<d masks voice d */
};

void opna_drum_reset(struct opna_drum *drum);
void opna_drum_set_rom(struct opna_drum *drum, const uint8_t *rom /* 8192 bytes */);
void opna_drum_writereg(struct opna_drum *drum, unsigned reg, unsigned val);
void opna_drum_mix(struct opna_drum *drum, int16_t *buf, unsigned samples);

#ifdef __cplusplus
}
#endif

#endif /* PFM_OPNA_DRUM_H_INCLUDED */
