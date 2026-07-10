/*
 * OPNA SSG synthesis + resampler. Ported from 98fmplayer/libopna/opnassg.c,
 * YMF288 path only. The resampling index arithmetic is kept identical to the
 * reference (9 half-samples per output sample, box average /9), but the 256-tap
 * sinc and the 1kB-per-sample memcpy are removed.
 */
#include "pfm/opna_ssg.h"

/* Captured from YMF288. 5-bit level index -> linear volume. */
/* NOT const: in .data (0-wait SRAM) — read per raw SSG sample (~4.5x/output). */
static PFM_RAMDATA uint16_t voltable[32] = {
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
/* (channel_level/tone_out_ymf288/tone_silent inlined into opna_ssg_mix) */

#define RINGMASK (PFM_SSG_RING - 1)
#define BUFINDEX(idx, n) ((((idx) >> 1) + (n)) & RINGMASK)

PFM_HOT void opna_ssg_mix(struct opna_ssg *ssg, struct opna_ssg_resampler *r,
                  int16_t *buf, unsigned samples) {
  /* Hoist everything derived only from the (mix-invariant) register file out of
     the 4.5x-oversampled inner loop. Only counters / lfsr / env level mutate. */
  const unsigned tp0 = tone_period(ssg, 0), tp1 = tone_period(ssg, 1), tp2 = tone_period(ssg, 2);
  const unsigned np = (unsigned)noise_period(ssg);
  const unsigned ep = (unsigned)env_period(ssg);
  const unsigned reg7 = ssg->regs[0x7];
  const bool cenv0 = chan_env(ssg, 0), cenv1 = chan_env(ssg, 1), cenv2 = chan_env(ssg, 2);
  const int tv0 = (tone_volume(ssg, 0) << 1) + 1, tv1 = (tone_volume(ssg, 1) << 1) + 1,
            tv2 = (tone_volume(ssg, 2) << 1) + 1;
  const unsigned tdis0 = reg7 & 1, tdis1 = (reg7 >> 1) & 1, tdis2 = (reg7 >> 2) & 1;
  const unsigned ndis0 = (reg7 >> 3) & 1, ndis1 = (reg7 >> 4) & 1, ndis2 = (reg7 >> 5) & 1;
  const unsigned tp[3] = {tp0, tp1, tp2};
  const bool cenv[3] = {cenv0, cenv1, cenv2};
  const int tv[3] = {tv0, tv1, tv2};
  const unsigned tdis[3] = {tdis0, tdis1, tdis2}, ndis[3] = {ndis0, ndis1, ndis2};

  for (unsigned i = 0; i < samples; i++) {
    /* generate the raw samples that fall in this output step (4 or 5) */
    unsigned ssg_samples = ((r->index + 9) >> 1) - (r->index >> 1);
    for (unsigned j = 0; j < ssg_samples; j++) {
      if (((++ssg->noise_counter) >> 1) >= np) {
        ssg->noise_counter = 0;
        ssg->lfsr |= (!((ssg->lfsr & 1) ^ ((ssg->lfsr >> 3) & 1))) << 17;
        ssg->lfsr >>= 1;
      }
      if (!ssg->env_holding) {
        if (++ssg->env_counter >= ep) {
          ssg->env_counter = 0;
          ssg->env_level++;
          if (ssg->env_level == 0x20) {
            ssg->env_level = 0;
            if (ssg->env_alt) ssg->env_att = !ssg->env_att;
            if (ssg->env_hld) { ssg->env_level = 0x1f; ssg->env_holding = true; }
          }
        }
      }
      const unsigned lfsr1 = ssg->lfsr & 1;
      const int envlvl = ssg->env_att ? ssg->env_level : 31 - ssg->env_level;
      const unsigned pos = BUFINDEX(r->index, j) * 3;
      for (int ch = 0; ch < 3; ch++) {
        if (++ssg->tone_counter[ch] >= tp[ch]) {
          ssg->tone_counter[ch] = 0;
          ssg->tone_out[ch] = !ssg->tone_out[ch];
        }
        int level = cenv[ch] ? envlvl : tv[ch];
        int16_t o;
        if (!(tdis[ch] && ndis[ch])) {
          bool toneout = tp[ch] < 8 ? true : ssg->tone_out[ch];
          bool on = (toneout || tdis[ch]) && (lfsr1 || ndis[ch]);
          o = on ? (int16_t)voltable[level] : (int16_t)(-(int)voltable[level]);
        } else {
          o = (int16_t)(voltable[level] * 2);
        }
        r->buf[pos + ch] = o;
      }
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
