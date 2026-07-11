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
  /* Hoist the mix-invariant register decode AND all mutable state (counters,
     LFSR, envelope, tone flip-flops, ring index) into locals. Without this the
     compiler reloads/stores them through the ssg pointer every raw sample,
     because it cannot prove the r->buf writes do not alias ssg. */
  const unsigned tp[3] = { tone_period(ssg, 0), tone_period(ssg, 1), tone_period(ssg, 2) };
  const unsigned np = (unsigned)noise_period(ssg);
  const unsigned ep = (unsigned)env_period(ssg);
  const unsigned reg7 = ssg->regs[0x7];
  const bool cenv[3] = { chan_env(ssg, 0), chan_env(ssg, 1), chan_env(ssg, 2) };
  const int tv[3] = { (tone_volume(ssg,0)<<1)+1, (tone_volume(ssg,1)<<1)+1, (tone_volume(ssg,2)<<1)+1 };
  const unsigned tdis[3] = { reg7 & 1, (reg7 >> 1) & 1, (reg7 >> 2) & 1 };
  const unsigned ndis[3] = { (reg7 >> 3) & 1, (reg7 >> 4) & 1, (reg7 >> 5) & 1 };
  const unsigned mask = ssg->mask;
  const bool ealt = ssg->env_alt, ehld = ssg->env_hld;

  uint8_t nc = ssg->noise_counter;
  uint32_t lfsr = ssg->lfsr;
  uint16_t ec = ssg->env_counter;
  uint8_t el = ssg->env_level;
  bool eatt = ssg->env_att, eholding = ssg->env_holding;
  uint16_t tc[3] = { ssg->tone_counter[0], ssg->tone_counter[1], ssg->tone_counter[2] };
  bool to[3] = { ssg->tone_out[0], ssg->tone_out[1], ssg->tone_out[2] };
  unsigned idx = r->index;
  int16_t *rbuf = r->buf;

  for (unsigned i = 0; i < samples; i++) {
    unsigned ssg_samples = ((idx + 9) >> 1) - (idx >> 1);
    for (unsigned j = 0; j < ssg_samples; j++) {
      if (((++nc) >> 1) >= np) {
        nc = 0;
        lfsr |= (!((lfsr & 1) ^ ((lfsr >> 3) & 1))) << 17;
        lfsr >>= 1;
      }
      if (!eholding) {
        if (++ec >= ep) {
          ec = 0;
          el++;
          if (el == 0x20) {
            el = 0;
            if (ealt) eatt = !eatt;
            if (ehld) { el = 0x1f; eholding = true; }
          }
        }
      }
      const unsigned lfsr1 = lfsr & 1;
      const int envlvl = eatt ? el : 31 - el;
      const unsigned pos = BUFINDEX(idx, j) * 3;
      for (int ch = 0; ch < 3; ch++) {
        if (++tc[ch] >= tp[ch]) { tc[ch] = 0; to[ch] = !to[ch]; }
        int level = cenv[ch] ? envlvl : tv[ch];
        int16_t o;
        if (!(tdis[ch] && ndis[ch])) {
          bool toneout = tp[ch] < 8 ? true : to[ch];
          bool on = (toneout || tdis[ch]) && (lfsr1 || ndis[ch]);
          o = on ? (int16_t)voltable[level] : (int16_t)(-(int)voltable[level]);
        } else {
          o = (int16_t)(voltable[level] * 2);
        }
        rbuf[pos + ch] = o;
      }
    }
    idx = (idx + 9) & (2 * PFM_SSG_RING - 1);

    /* box-average /9 (rectangular FIR matching the 9/2 ratio) */
    int32_t sample = 0;
    for (int ch = 0; ch < 3; ch++) {
      unsigned ind = (idx & 1) ? BUFINDEX(idx, 5) : BUFINDEX(idx, 0);
      int32_t out = rbuf[ind * 3 + ch];
      for (int s = 0; s < 4; s++) out += rbuf[BUFINDEX(idx, s + 1) * 3 + ch] * 2;
      out /= 9;
      if (!(mask & (1u << ch))) sample += out;
    }
    int32_t lo = buf[i * 2 + 0] + sample;
    int32_t ro = buf[i * 2 + 1] + sample;
    buf[i * 2 + 0] = (int16_t)pfm_clamp16(lo);
    buf[i * 2 + 1] = (int16_t)pfm_clamp16(ro);
  }

  ssg->noise_counter = nc;
  ssg->lfsr = lfsr;
  ssg->env_counter = ec;
  ssg->env_level = el;
  ssg->env_att = eatt;
  ssg->env_holding = eholding;
  ssg->tone_counter[0] = tc[0]; ssg->tone_counter[1] = tc[1]; ssg->tone_counter[2] = tc[2];
  ssg->tone_out[0] = to[0]; ssg->tone_out[1] = to[1]; ssg->tone_out[2] = to[2];
  r->index = idx;
}
