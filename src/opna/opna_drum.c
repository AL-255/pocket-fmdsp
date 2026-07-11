/*
 * OPNA rhythm (drum) block. The authentic YM2608 rhythm decodes a 4-bit ADPCM-A
 * ROM, but that ROM isn't available here, so we play the FMPMDE 44.1kHz PCM
 * recordings of the 6 rhythm voices (embedded in flash, opna_drum_pcm.h),
 * resampled to the mix rate with linear interpolation. The register interface
 * (key-on 0x10, total level 0x11, per-voice level/pan 0x18..0x1d) and the OPNA
 * rhythm volume law are unchanged, so songs' drum levels behave as before.
 */
#include "pfm/opna_drum.h"
#include "pfm/pfm_config.h"
#include "opna_drum_pcm.h"   /* generated: drum_pcm[6] + PFM_DRUM_PCM_RATE */

void opna_drum_reset(struct opna_drum *drum) {
  for (unsigned i = 0; i < sizeof(*drum); i++) ((uint8_t *)drum)[i] = 0;
  for (int d = 0; d < 6; d++) {
    drum->v[d].data = drum_pcm[d].data;
    drum->v[d].len = drum_pcm[d].len;
    drum->v[d].playing = false;
  }
  /* Q16.16 read step = sample_rate / mix_rate (advance per output sample). */
  drum->step = (uint32_t)(((uint64_t)PFM_DRUM_PCM_RATE << 16) + (PFM_MIX_RATE >> 1)) / PFM_MIX_RATE;
}

void opna_drum_set_rom(struct opna_drum *drum, const uint8_t *rom) {
  (void)drum; (void)rom; /* no external ROM: samples are embedded PCM */
}

static void drum_keyon(struct opna_drum *drum, int d) {
  struct opna_drum_voice *v = &drum->v[d];
  v->pos = 0;
  v->playing = v->data && v->len;
}

void opna_drum_writereg(struct opna_drum *drum, unsigned reg, unsigned val) {
  val &= 0xff;
  switch (reg) {
  case 0x10:
    for (int d = 0; d < 6; d++) {
      if (val & (1 << d)) {
        if (val & 0x80) drum->v[d].playing = false;  /* dump = key-off */
        else drum_keyon(drum, d);
      }
    }
    break;
  case 0x11:
    drum->total_level = val & 0x3f;
    break;
  case 0x18: case 0x19: case 0x1a:
  case 0x1b: case 0x1c: case 0x1d: {
    int d = reg - 0x18;
    drum->v[d].left = val & 0x80;
    drum->v[d].right = val & 0x40;
    drum->v[d].level = val & 0x1f;
    break;
  }
  default:
    break;
  }
}

PFM_HOT void opna_drum_mix(struct opna_drum *drum, int16_t *buf, unsigned samples) {
  const uint32_t step = drum->step;
  int any = 0;
  for (int d = 0; d < 6; d++) if (drum->v[d].playing) { any = 1; break; }
  if (!any) return;   /* nothing keyed on -> don't touch the buffer at all */
  for (unsigned i = 0; i < samples; i++) {
    int32_t lo = buf[i * 2 + 0];
    int32_t ro = buf[i * 2 + 1];
    for (int d = 0; d < 6; d++) {
      struct opna_drum_voice *v = &drum->v[d];
      if (!v->playing) continue;
      unsigned idx = v->pos >> 16;
      if (idx >= v->len) { v->playing = false; continue; }
      /* linear interpolation between adjacent PCM samples */
      int s0 = v->data[idx];
      int s1 = (idx + 1 < v->len) ? v->data[idx + 1] : s0;
      int frac = (int)(v->pos & 0xffff);
      int co = s0 + (((s1 - s0) * frac) >> 16);      /* 16-bit sample */
      /* OPNA rhythm volume law (per-voice + total level). The +4 in the final
         shift (vs the 12-bit ADPCM path's +1) rescales the 16-bit PCM to the
         same output level. */
      unsigned level = (v->level ^ 0x1f) + (drum->total_level ^ 0x3f);
      co *= 15 - (int)(level & 7);
      co >>= 5 + (level >> 3);
      if (!(drum->mask & (1u << d))) {
        if (v->left) lo += co;
        if (v->right) ro += co;
      }
      v->pos += step;
    }
    buf[i * 2 + 0] = (int16_t)pfm_clamp16(lo);
    buf[i * 2 + 1] = (int16_t)pfm_clamp16(ro);
  }
}
