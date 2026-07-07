#ifndef PFM_OPNA_SSG_H_INCLUDED
#define PFM_OPNA_SSG_H_INCLUDED

/*
 * OPNA SSG (PSG / AY-3-8910-style) block: 3 square tones + noise + envelope.
 * Reimplemented from 98fmplayer/libopna/opnassg.c, YMF288 output/resampling
 * only (the reset default). The OPNA-analog HPF + 256-tap sinc are dropped, and
 * the reference's per-output-sample 1kB memcpy is eliminated.
 */

#include "pfm/pfm_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Resampler ring length in half-raw-samples. Kept equal to the reference (128)
   so the 9/2 resampling arithmetic is bit-identical; no tail-duplication copy. */
#define PFM_SSG_RING 128

struct opna_ssg {
  uint8_t regs[0x10];
  uint16_t tone_counter[3];
  bool tone_out[3];
  uint8_t noise_counter;
  uint32_t lfsr;         /* 17-bit noise LFSR */
  uint16_t env_counter;
  uint8_t env_level;     /* 5-bit */
  bool env_att, env_alt, env_hld, env_holding;
  unsigned mask;         /* 1<<ch masks channel ch */
};

struct opna_ssg_resampler {
  int16_t buf[PFM_SSG_RING * 3]; /* 3 channels per ring slot */
  unsigned index;                /* counts half-raw-samples, wraps at 2*RING */
};

void opna_ssg_reset(struct opna_ssg *ssg);
void opna_ssg_resampler_reset(struct opna_ssg_resampler *r);
void opna_ssg_writereg(struct opna_ssg *ssg, unsigned reg, unsigned val);
unsigned opna_ssg_readreg(const struct opna_ssg *ssg, unsigned reg);
/* Resample to PFM_MIX_RATE and accumulate into interleaved-stereo buf. */
void opna_ssg_mix(struct opna_ssg *ssg, struct opna_ssg_resampler *r,
                  int16_t *buf, unsigned samples);

#ifdef __cplusplus
}
#endif

#endif /* PFM_OPNA_SSG_H_INCLUDED */
