/*
 * OPNA SSG synthesis + resampler. Ported from 98fmplayer/libopna/opnassg.c,
 * YMF288 path only. The resampling index arithmetic is kept identical to the
 * reference (9 half-samples per output sample, box average /9), but the 256-tap
 * sinc and the 1kB-per-sample memcpy are removed.
 */
#include "pfm/opna_ssg.h"

/* Captured from YMF288. 5-bit level index -> linear volume. */
static const uint16_t voltable[32] = {
  0, 0, 0, 0, 4, 8, 12, 16,
  20, 24, 28, 32, 36, 44, 52, 64,
  76, 92, 108, 128, 152, 180, 216, 256,
  304, 360, 428, 512, 608, 720, 856, 1020,
};

void opna_ssg_reset(struct opna_ssg *ssg) {
  for (unsigned i = 0; i < sizeof(*ssg); i++) ((uint8_t *)ssg)[i] = 0;
}

void opna_ssg_resampler_reset(struct opna_ssg_resampler *r) {
  for (unsigned i = 0; i < PFM_SSG_RING * 3; i++) r->buf[i] = 0;
  r->index = 0;
}

void opna_ssg_writereg(struct opna_ssg *ssg, unsigned reg, unsigned val) {
  if (reg > 0xfu) return;
  val &= 0xff;
  ssg->regs[reg] = val;
  if (reg == 0xd) {
    ssg->env_att = ssg->regs[0xd] & 0x4;
    if (ssg->regs[0xd] & 0x8) {
      ssg->env_alt = ssg->regs[0xd] & 0x2;
      ssg->env_hld = ssg->regs[0xd] & 0x1;
    } else {
      ssg->env_alt = ssg->env_att;
      ssg->env_hld = true;
    }
    ssg->env_holding = false;
    ssg->env_level = 0;
    ssg->env_counter = 0;
  }
}

unsigned opna_ssg_readreg(const struct opna_ssg *ssg, unsigned reg) {
  if (reg > 0xfu) return 0xff;
  return ssg->regs[reg];
}

static unsigned tone_period(const struct opna_ssg *ssg, int ch) {
  return ssg->regs[0 + ch * 2] | ((ssg->regs[1 + ch * 2] & 0xf) << 8);
}
static int noise_period(const struct opna_ssg *ssg) { return ssg->regs[0x6] & 0x1f; }
static int env_period(const struct opna_ssg *ssg) {
  return (ssg->regs[0xc] << 8) | ssg->regs[0xb];
}
static bool chan_env(const struct opna_ssg *ssg, int ch) { return ssg->regs[0x8 + ch] & 0x10; }
static int tone_volume(const struct opna_ssg *ssg, int ch) { return ssg->regs[0x8 + ch] & 0xf; }
static int env_level_val(const struct opna_ssg *ssg) {
  return ssg->env_att ? ssg->env_level : 31 - ssg->env_level;
}
static int channel_level(const struct opna_ssg *ssg, int ch) {
  return chan_env(ssg, ch) ? env_level_val(ssg) : (tone_volume(ssg, ch) << 1) + 1;
}
static bool tone_out_ymf288(const struct opna_ssg *ssg, int ch) {
  unsigned reg = ssg->regs[0x7] >> ch;
  bool toneout = tone_period(ssg, ch) < 8 ? true : ssg->tone_out[ch];
  return (toneout || (reg & 0x1)) && ((ssg->lfsr & 1) || (reg & 0x8));
}
static bool tone_silent(const struct opna_ssg *ssg, int ch) {
  unsigned reg = ssg->regs[0x7] >> ch;
  return (reg & 0x1) && (reg & 0x8);
}

/* Generate one raw SSG sample (3 channels), advancing all dividers. */
static void ssg_raw_sample(struct opna_ssg *ssg, int16_t out[3]) {
  if (((++ssg->noise_counter) >> 1) >= (unsigned)noise_period(ssg)) {
    ssg->noise_counter = 0;
    ssg->lfsr |= (!((ssg->lfsr & 1) ^ ((ssg->lfsr >> 3) & 1))) << 17;
    ssg->lfsr >>= 1;
  }
  if (!ssg->env_holding) {
    if (++ssg->env_counter >= (unsigned)env_period(ssg)) {
      ssg->env_counter = 0;
      ssg->env_level++;
      if (ssg->env_level == 0x20) {
        ssg->env_level = 0;
        if (ssg->env_alt) ssg->env_att = !ssg->env_att;
        if (ssg->env_hld) {
          ssg->env_level = 0x1f;
          ssg->env_holding = true;
        }
      }
    }
  }
  for (int ch = 0; ch < 3; ch++) {
    if (++ssg->tone_counter[ch] >= tone_period(ssg, ch)) {
      ssg->tone_counter[ch] = 0;
      ssg->tone_out[ch] = !ssg->tone_out[ch];
    }
    int level = channel_level(ssg, ch);
    if (!tone_silent(ssg, ch)) {
      out[ch] = tone_out_ymf288(ssg, ch) ? voltable[level] : -(int)voltable[level];
    } else {
      out[ch] = voltable[level] * 2;
    }
  }
}

#define RINGMASK (PFM_SSG_RING - 1)
#define BUFINDEX(idx, n) ((((idx) >> 1) + (n)) & RINGMASK)

void opna_ssg_mix(struct opna_ssg *ssg, struct opna_ssg_resampler *r,
                  int16_t *buf, unsigned samples) {
  for (unsigned i = 0; i < samples; i++) {
    /* generate the raw samples that fall in this output step (4 or 5) */
    unsigned ssg_samples = ((r->index + 9) >> 1) - (r->index >> 1);
    for (unsigned j = 0; j < ssg_samples; j++) {
      int16_t s[3];
      ssg_raw_sample(ssg, s);
      unsigned pos = BUFINDEX(r->index, j);
      r->buf[pos * 3 + 0] = s[0];
      r->buf[pos * 3 + 1] = s[1];
      r->buf[pos * 3 + 2] = s[2];
    }
    r->index += 9;
    r->index &= (2 * PFM_SSG_RING) - 1;

    /* box-average /9 (rectangular FIR matching the 9/2 ratio) */
    int32_t sample = 0;
    for (int ch = 0; ch < 3; ch++) {
      unsigned ind = (r->index & 1) ? BUFINDEX(r->index, 5) : BUFINDEX(r->index, 0);
      int32_t out = r->buf[ind * 3 + ch];
      for (int s = 0; s < 4; s++) out += r->buf[BUFINDEX(r->index, s + 1) * 3 + ch] * 2;
      out /= 9;
      if (!(ssg->mask & (1u << ch))) sample += out;
    }

    int32_t lo = buf[i * 2 + 0] + sample;
    int32_t ro = buf[i * 2 + 1] + sample;
    buf[i * 2 + 0] = (int16_t)pfm_clamp16(lo);
    buf[i * 2 + 1] = (int16_t)pfm_clamp16(ro);
  }
}
