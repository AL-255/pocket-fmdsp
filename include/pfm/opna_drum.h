#ifndef PFM_OPNA_DRUM_H_INCLUDED
#define PFM_OPNA_DRUM_H_INCLUDED

/*
 * OPNA rhythm block: 6 drum voices (BD/SD/TOP/HH/TOM/RIM). The authentic YM2608
 * rhythm is a 4-bit ADPCM-A ROM, but that ROM isn't available here, so we play
 * the FMPMDE 44.1kHz PCM recordings of those 6 samples (embedded in flash via
 * firmware/tools/gen_drumpcm.py -> opna_drum_pcm.h), resampled to the mix rate.
 * Each voice keeps only a fractional read position; sample data lives in flash.
 */

#include "pfm/pfm_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PFM_DRUM_ROM_SIZE 0x2000 /* legacy: kept so ADPCM-ROM callers still build */

struct opna_drum_voice {
  const int16_t *data;   /* PCM sample (flash), NULL if unavailable */
  unsigned len;          /* sample count */
  uint32_t pos;          /* Q16.16 read position into data */
  bool playing;
  uint8_t level;         /* 0..31 per-voice attenuation */
  bool left, right;
};

struct opna_drum {
  struct opna_drum_voice v[6];
  unsigned total_level;    /* 0..63 */
  unsigned mask;           /* 1<<d masks voice d */
  uint32_t step;           /* Q16.16 read advance per output sample (rate ratio) */
};

void opna_drum_reset(struct opna_drum *drum);
/* Legacy no-op: the drum now plays embedded PCM, not an external ADPCM ROM. */
void opna_drum_set_rom(struct opna_drum *drum, const uint8_t *rom);
void opna_drum_writereg(struct opna_drum *drum, unsigned reg, unsigned val);
void opna_drum_mix(struct opna_drum *drum, int16_t *buf, unsigned samples);

#ifdef __cplusplus
}
#endif

#endif /* PFM_OPNA_DRUM_H_INCLUDED */
