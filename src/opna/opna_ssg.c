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
  /* Reduced-oversampling resampler: the tone/noise/env counters still advance at
     the native rate (so pitch is exact), but instead of writing every raw sample
     into a ring and running a 9-tap sliding box, we block-average the raw samples
     that fall in each output step (4 or 5) directly. Drops the whole ring + FIR
     memory traffic; slightly more aliasing on the square/noise channels. */
  static const int32_t inv[6] = { 0, 0, 0, 0, 8192, 6554 }; /* Q15 1/4, 1/5 */
  const unsigned tp0 = tone_period(ssg, 0), tp1 = tone_period(ssg, 1), tp2 = tone_period(ssg, 2);
  const unsigned np = (unsigned)noise_period(ssg);
  const unsigned ep = (unsigned)env_period(ssg);
  const unsigned reg7 = ssg->regs[0x7];
  const bool ce0 = chan_env(ssg, 0), ce1 = chan_env(ssg, 1), ce2 = chan_env(ssg, 2);
  const bool anyenv = ce0 || ce1 || ce2;
  /* Fixed (non-env) channel volumes precomputed -> no per-tick voltable[] SRAM
     read in the common melody case; env channels use the per-tick venv below. */
  const int volc0 = voltable[(tone_volume(ssg,0)<<1)+1];
  const int volc1 = voltable[(tone_volume(ssg,1)<<1)+1];
  const int volc2 = voltable[(tone_volume(ssg,2)<<1)+1];
  const unsigned td0 = reg7 & 1, td1 = (reg7 >> 1) & 1, td2 = (reg7 >> 2) & 1;
  const unsigned nd0 = (reg7 >> 3) & 1, nd1 = (reg7 >> 4) & 1, nd2 = (reg7 >> 5) & 1;
  /* tforce = period<8 forces the tone gate high; bothdis = tone+noise disabled. */
  const bool tf0 = tp0 < 8, tf1 = tp1 < 8, tf2 = tp2 < 8;
  const bool bd0 = td0 && nd0, bd1 = td1 && nd1, bd2 = td2 && nd2;
  const unsigned mask = ssg->mask;
  const bool ealt = ssg->env_alt, ehld = ssg->env_hld;

  uint8_t nc = ssg->noise_counter;
  uint32_t lfsr = ssg->lfsr;
  uint16_t ec = ssg->env_counter;
  uint8_t el = ssg->env_level;
  bool eatt = ssg->env_att, eholding = ssg->env_holding;
  uint16_t tc0 = ssg->tone_counter[0], tc1 = ssg->tone_counter[1], tc2 = ssg->tone_counter[2];
  /* Pack the 3 tone-output flags into one word: toggling becomes a register XOR
     instead of a stack load-modify-store (the disasm's hottest spill). */
  unsigned topack = (ssg->tone_out[0] ? 1u : 0) | (ssg->tone_out[1] ? 2u : 0) | (ssg->tone_out[2] ? 4u : 0);
  unsigned idx = r->index;

  for (unsigned i = 0; i < samples; i++) {
    unsigned ticks = ((idx + 9) >> 1) - (idx >> 1); /* 4 or 5 */
    idx = (idx + 9) & (2 * PFM_SSG_RING - 1);
    int32_t s0 = 0, s1 = 0, s2 = 0;
    for (unsigned k = 0; k < ticks; k++) {
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
      const int venv = anyenv ? (int)voltable[eatt ? el : 31 - el] : 0;
      if (++tc0 >= tp0) { tc0 = 0; topack ^= 1u; }
      if (++tc1 >= tp1) { tc1 = 0; topack ^= 2u; }
      if (++tc2 >= tp2) { tc2 = 0; topack ^= 4u; }
      int v0 = ce0 ? venv : volc0;
      int v1 = ce1 ? venv : volc1;
      int v2 = ce2 ? venv : volc2;
      if (!bd0) s0 += ((tf0 || (topack & 1u) || td0) && (lfsr1 || nd0)) ? v0 : -v0;
      else s0 += v0 * 2;
      if (!bd1) s1 += ((tf1 || (topack & 2u) || td1) && (lfsr1 || nd1)) ? v1 : -v1;
      else s1 += v1 * 2;
      if (!bd2) s2 += ((tf2 || (topack & 4u) || td2) && (lfsr1 || nd2)) ? v2 : -v2;
      else s2 += v2 * 2;
    }
    int32_t rr = inv[ticks];
    int32_t sample = 0;
    if (!(mask & 1u)) sample += (s0 * rr) >> 15;
    if (!(mask & 2u)) sample += (s1 * rr) >> 15;
    if (!(mask & 4u)) sample += (s2 * rr) >> 15;
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
  ssg->tone_counter[0] = tc0; ssg->tone_counter[1] = tc1; ssg->tone_counter[2] = tc2;
  ssg->tone_out[0] = topack & 1u; ssg->tone_out[1] = (topack >> 1) & 1u; ssg->tone_out[2] = (topack >> 2) & 1u;
  r->index = idx;
}
