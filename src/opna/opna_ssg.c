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

/* Closed-form block sum for one square-tone channel over `ticks` (4-5) native
   ticks, used only on the fast path (channel noise disabled, no hardware env).
   The tone gate toggles deterministically, so instead of iterating tick-by-tick
   we compute the whole block's contribution and the post-block counter/output
   directly. Updates *tcp (counter) and *tobp (tone-out bit, 0/1) exactly as the
   per-tick loop would. Verified bit-exact vs the reference loop over 2M chained
   random configs (output AND all state). tp>=8 (!tf) can wrap at most once in a
   4-5 tick block, so that common path is division-free. */
static inline int32_t
ssg_tone_block(unsigned *tcp, unsigned *tobp,
                                     unsigned tp, unsigned ticks,
                                     unsigned tf, unsigned bd, int v) {
  /* m1 = tick of the first wrap. On wrap the hw counter resets to 0 (not to
     c-tp), so when the counter already exceeds the period (c>=tp-1, e.g. after
     the song shrinks the period) the very first tick wraps: m1=1. tp>=8 (!tf)
     can then wrap at most once in a 4-5 tick block. */
  unsigned c = *tcp, ob = *tobp, W, ntc, m1 = 1;
  if (tp == 0) {                               /* ++tc>=0 always: wraps every tick */
    W = ticks; ntc = 0;
  } else {
    m1 = (c + 1 >= tp) ? 1u : (tp - c);
    if (ticks < m1) { W = 0; ntc = c + ticks; }        /* no wrap this block */
    else {
      unsigned R = ticks - m1;
      if (!tf) { W = 1; ntc = R; }                     /* tp>=8: R<tp, single wrap */
      else { unsigned e = R / tp; W = 1 + e; ntc = R - e * tp; } /* tp<8: multi-wrap */
    }
  }
  *tcp = ntc;
  *tobp = ob ^ (W & 1u);
  if (bd) return 2 * v * (int)ticks;           /* tone+noise disabled: +2v/tick */
  if (tf) return v * (int)ticks;               /* gate forced high: +v/tick */
  int ones;                                    /* tp>=8: gate follows tone-out, W<=1 */
  if (!W) ones = ob ? (int)ticks : 0;
  else ones = ob ? (int)(m1 - 1) : (int)(ticks - m1 + 1);
  return v * (2 * ones - (int)ticks);
}

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
  unsigned tc0 = ssg->tone_counter[0], tc1 = ssg->tone_counter[1], tc2 = ssg->tone_counter[2];
  /* Pack the 3 tone-output flags into one word: toggling becomes a register XOR
     instead of a stack load-modify-store (the disasm's hottest spill). */
  unsigned topack = (ssg->tone_out[0] ? 1u : 0) | (ssg->tone_out[1] ? 2u : 0) | (ssg->tone_out[2] ? 4u : 0);
  unsigned idx = r->index;

  /* Per-channel fast path. A channel is "simple" when its noise is disabled and
     it doesn't use the hardware envelope -- then its whole block contribution has
     a closed form (ssg_tone_block) and it needs no per-tick work. ~73% of channel
     activity across the library is simple even though whole-call all-simple frames
     are only ~24%, so closing the form per channel (not per call) is the real win.
     The shared noise LFSR + env still advance in the per-tick loop (their state is
     needed regardless); only the *complex* channels (noise or env active) keep
     their per-tick gate there. simpleN is loop-invariant, so its branches are
     predictable / hoistable. Bit-exact vs the original per-tick loop (108-song
     byte-diff + QEMU). */
  const unsigned simple0 = nd0 && !ce0, simple1 = nd1 && !ce1, simple2 = nd2 && !ce2;
  unsigned tob0 = topack & 1u, tob1 = (topack >> 1) & 1u, tob2 = (topack >> 2) & 1u;
  for (unsigned i = 0; i < samples; i++) {
    unsigned ticks = ((idx + 9) >> 1) - (idx >> 1); /* 4 or 5 */
    idx = (idx + 9) & (2 * PFM_SSG_RING - 1);
    int32_t s0 = simple0 ? ssg_tone_block(&tc0, &tob0, tp0, ticks, tf0, bd0, volc0) : 0;
    int32_t s1 = simple1 ? ssg_tone_block(&tc1, &tob1, tp1, ticks, tf1, bd1, volc1) : 0;
    int32_t s2 = simple2 ? ssg_tone_block(&tc2, &tob2, tp2, ticks, tf2, bd2, volc2) : 0;
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
      if (!simple0) {
        if (++tc0 >= tp0) { tc0 = 0; tob0 ^= 1u; }
        int v0 = ce0 ? venv : volc0;
        if (!bd0) s0 += ((tf0 || tob0 || td0) && (lfsr1 || nd0)) ? v0 : -v0;
        else s0 += v0 * 2;
      }
      if (!simple1) {
        if (++tc1 >= tp1) { tc1 = 0; tob1 ^= 1u; }
        int v1 = ce1 ? venv : volc1;
        if (!bd1) s1 += ((tf1 || tob1 || td1) && (lfsr1 || nd1)) ? v1 : -v1;
        else s1 += v1 * 2;
      }
      if (!simple2) {
        if (++tc2 >= tp2) { tc2 = 0; tob2 ^= 1u; }
        int v2 = ce2 ? venv : volc2;
        if (!bd2) s2 += ((tf2 || tob2 || td2) && (lfsr1 || nd2)) ? v2 : -v2;
        else s2 += v2 * 2;
      }
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
  topack = tob0 | (tob1 << 1) | (tob2 << 2);

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
