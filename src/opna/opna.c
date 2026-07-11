/* Top-level OPNA: register fan-out + mix, ported from libopna/opna.c. */
#include "pfm/opna.h"
#include "pfm/pfm_prof.h"
#include <string.h>

void opna_reset(struct opna *opna) {
  opna_fm_reset(&opna->fm);
  opna_ssg_reset(&opna->ssg);
  opna_ssg_resampler_reset(&opna->resampler);
  opna_drum_reset(&opna->drum);
  opna->mask = 0;
  opna->generated_frames = 0;
}

void opna_writereg(struct opna *opna, unsigned reg, unsigned val) {
  val &= 0xff;
  opna_fm_writereg(&opna->fm, reg, val);
  opna_ssg_writereg(&opna->ssg, reg, val);
  opna_drum_writereg(&opna->drum, reg, val);
}

unsigned opna_readreg(const struct opna *opna, unsigned reg) {
  if (reg > 0xfu) return 0xff;
  return opna_ssg_readreg(&opna->ssg, reg);
}

uint32_t pfm_prof_cyc[PFM_PROF_N];
uint32_t (*pfm_prof_clock)(void);

PFM_HOT void opna_mix(struct opna *opna, int16_t *buf, unsigned samples) {
  uint32_t t;
  memset(buf, 0, (size_t)samples * 2 * sizeof(int16_t));
  /* Fully-masked groups are bypassed entirely (no synthesis) so muting a voice in
     Settings frees its CPU. SSG especially: its tick loop ran regardless of mask
     before. Masks are all-or-nothing here (only the mute UI sets them). */
  if ((opna->fm.mask & 0x3f) != 0x3f) {
    t = pfm_prof_begin(); opna_fm_mix(&opna->fm, buf, samples);
    pfm_prof_end(PFM_PROF_FM, t);
  }
  if (opna->ssg.mask != 0x7) {
    t = pfm_prof_begin(); opna_ssg_mix(&opna->ssg, &opna->resampler, buf, samples);
    pfm_prof_end(PFM_PROF_SSG, t);
  }
  if (opna->drum.mask != 0x3f) {
    t = pfm_prof_begin(); opna_drum_mix(&opna->drum, buf, samples);
    pfm_prof_end(PFM_PROF_DRUM, t);
  }
  opna->generated_frames += samples;
}

unsigned opna_get_mask(const struct opna *opna) { return opna->mask; }

void opna_set_mask(struct opna *opna, unsigned mask) {
  opna->mask = mask & 0xffffu;
  opna->fm.mask = mask & 0x3f;
  opna->ssg.mask = (mask >> 6) & 0x7;
  opna->drum.mask = (mask >> 9) & 0x3f;
}
