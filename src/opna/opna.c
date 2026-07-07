/* Top-level OPNA: register fan-out + mix, ported from libopna/opna.c. */
#include "pfm/opna.h"
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

void opna_mix(struct opna *opna, int16_t *buf, unsigned samples) {
  memset(buf, 0, (size_t)samples * 2 * sizeof(int16_t));
  opna_fm_mix(&opna->fm, buf, samples);
  opna_ssg_mix(&opna->ssg, &opna->resampler, buf, samples);
  opna_drum_mix(&opna->drum, buf, samples);
  opna->generated_frames += samples;
}

unsigned opna_get_mask(const struct opna *opna) { return opna->mask; }

void opna_set_mask(struct opna *opna, unsigned mask) {
  opna->mask = mask & 0xffffu;
  opna->fm.mask = mask & 0x3f;
  opna->ssg.mask = (mask >> 6) & 0x7;
  opna->drum.mask = (mask >> 9) & 0x3f;
}
