#ifndef PFM_OPNA_H_INCLUDED
#define PFM_OPNA_H_INCLUDED

/* Top-level OPNA chip: fans register writes and mixing to FM / SSG / drum. */

#include "pfm/opna_fm.h"
#include "pfm/opna_ssg.h"
#include "pfm/opna_drum.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
  PFM_CHAN_FM_1 = 0x0001, PFM_CHAN_FM_2 = 0x0002, PFM_CHAN_FM_3 = 0x0004,
  PFM_CHAN_FM_4 = 0x0008, PFM_CHAN_FM_5 = 0x0010, PFM_CHAN_FM_6 = 0x0020,
  PFM_CHAN_SSG_1 = 0x0040, PFM_CHAN_SSG_2 = 0x0080, PFM_CHAN_SSG_3 = 0x0100,
  PFM_CHAN_DRUM_ALL = 0x7e00,
};

struct opna {
  struct opna_fm fm;
  struct opna_ssg ssg;
  struct opna_drum drum;
  struct opna_ssg_resampler resampler;
  unsigned mask;
  uint64_t generated_frames;
};

void opna_reset(struct opna *opna);
void opna_writereg(struct opna *opna, unsigned reg, unsigned val);
unsigned opna_readreg(const struct opna *opna, unsigned reg);
/* Overwrites `samples` interleaved-stereo int16 frames (caller need not zero). */
void opna_mix(struct opna *opna, int16_t *buf, unsigned samples);
void opna_set_mask(struct opna *opna, unsigned mask);
unsigned opna_get_mask(const struct opna *opna);

#ifdef __cplusplus
}
#endif

#endif /* PFM_OPNA_H_INCLUDED */
