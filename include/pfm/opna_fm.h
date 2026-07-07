#ifndef PFM_OPNA_FM_H_INCLUDED
#define PFM_OPNA_FM_H_INCLUDED

/*
 * OPNA FM block: 6 channels x 4 operators, YM2608/YMF288 accurate integer
 * synthesis. Reimplemented from 98fmplayer/libopna/opnafm.c with the optional
 * hi-res sin/env paths removed (they were disabled upstream by default).
 */

#include "pfm/pfm_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PFM_FM_ENV_MAX 1023

enum {
  PFM_ENV_ATTACK,
  PFM_ENV_DECAY,
  PFM_ENV_SUSTAIN,
  PFM_ENV_RELEASE,
  PFM_ENV_OFF,
};

/* One FM operator ("slot"). */
struct opna_fm_slot {
  uint32_t phase;        /* 20-bit phase accumulator */
  uint16_t env;          /* 10-bit attenuation (0..1023) */
  uint16_t env_count;
  uint8_t env_state;
  /* rate precomputed on any param/keycode change */
  uint8_t rate_shifter;
  uint8_t rate_selector;
  uint8_t rate_mul;
  uint8_t tl;            /* total level 0..127 */
  uint8_t sl;            /* sustain level 0..15 */
  uint8_t ar, dr, sr, rr;
  uint8_t mul, det, ks;
  uint8_t keycode;       /* 5 bits, derived from blk/fnum */
  bool keyon_ext;        /* written via reg 0x28 */
  bool keyon;            /* env-synced */
  int16_t prevout;       /* last operator output (feedback/algorithm routing) */
};

struct opna_fm_channel {
  struct opna_fm_slot slot[4];
  uint16_t fbmem;        /* slot0 feedback memory (2-sample avg) */
  uint16_t alg_mem;      /* 1-sample-delayed accumulator (algorithms 1/3/5/6/7) */
  uint8_t alg;
  uint8_t fb;
  uint16_t fnum;
  uint8_t blk;
};

struct opna_fm {
  struct opna_fm_channel channel[6];
  uint8_t blkfnum_h;     /* latched high fnum/blk byte (0xA4-0xA6) */
  struct {
    uint16_t fnum[3];
    uint8_t blk[3];
    uint8_t mode;        /* CH3 mode: 0 normal, 1 CSM, 2 SE */
  } ch3;
  uint8_t env_div3;      /* envelope runs once per 3 output samples */
  bool lselect[6];
  bool rselect[6];
  unsigned mask;         /* 1<<c masks channel c */
};

void opna_fm_reset(struct opna_fm *fm);
void opna_fm_writereg(struct opna_fm *fm, unsigned reg, unsigned val);
/* Accumulate `samples` interleaved-stereo frames into buf (adds, then clamps). */
void opna_fm_mix(struct opna_fm *fm, int16_t *buf, unsigned samples);

#ifdef __cplusplus
}
#endif

#endif /* PFM_OPNA_FM_H_INCLUDED */
