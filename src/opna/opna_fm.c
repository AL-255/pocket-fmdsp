/*
 * OPNA FM synthesis. Ported from 98fmplayer/libopna/opnafm.c, hi-res paths
 * removed, integer-only. Behaviour matches the reference standard-res path.
 */
#include "pfm/opna_fm.h"
#include "opna_tables.h"

enum {
  CH3_MODE_NORMAL = 0,
  CH3_MODE_CSM = 1,
  CH3_MODE_SE = 2,
};

static void opna_fm_slot_reset(struct opna_fm_slot *slot) {
  slot->env = PFM_FM_ENV_MAX;
  slot->env_state = PFM_ENV_RELEASE;
}

static void opna_fm_chan_reset(struct opna_fm_channel *chan) {
  for (int i = 0; i < 4; i++) opna_fm_slot_reset(&chan->slot[i]);
}

void opna_fm_reset(struct opna_fm *fm) {
  for (unsigned i = 0; i < sizeof(*fm); i++) ((uint8_t *)fm)[i] = 0;
  for (int i = 0; i < 6; i++) {
    opna_fm_chan_reset(&fm->channel[i]);
    fm->lselect[i] = true;
    fm->rselect[i] = true;
  }
  fm->ch3.mode = CH3_MODE_NORMAL;
}

/* Operator output: log-sine + attenuation, converted to linear via exptable.
   Max magnitude 2042<<2 = 8168. */
static int16_t opna_fm_slotout(struct opna_fm_slot *slot, int16_t modulation) {
  unsigned pind = (slot->phase >> 10);
  pind += (unsigned)(modulation >> 1);
  bool minus = pind & (1u << (PFM_LOGSINTABLEBIT + 1));
  bool reverse = pind & (1u << PFM_LOGSINTABLEBIT);
  if (reverse) pind = ~pind;
  pind &= (1u << PFM_LOGSINTABLEBIT) - 1;

  int logout = pfm_logsintable[pind];
  logout += slot->env << 2;
  logout += slot->tl << 5;

  int selector = logout & ((1 << PFM_EXPTABLEBIT) - 1);
  int shifter = logout >> PFM_EXPTABLEBIT;
  if (shifter > 13) shifter = 13;

  int16_t out = (int16_t)((pfm_exptable[selector] << 2) >> shifter);
  if (minus) out = -out;
  slot->prevout = out;
  return out;
}

static unsigned blkfnum2freq(unsigned blk, unsigned fnum) {
  return (fnum << blk) >> 1;
}

#define F(n) (!!(fnum & (1 << ((n)-1))))
static unsigned blkfnum2keycode(unsigned blk, unsigned fnum) {
  unsigned keycode = blk << 2;
  keycode |= F(11) << 1;
  keycode |= (F(11) && (F(10) || F(9) || F(8))) ||
             ((!F(11)) && F(10) && F(9) && F(8));
  return keycode;
}
#undef F

/* Phase increment for one slot at a given source freq. This is invariant
   between register writes, so we precompute it (opna_fm_chan_calc_phase_inc)
   instead of recomputing per output sample. Formula identical to the old
   opna_fm_slot_phase body. */
static uint32_t slot_phase_inc(const struct opna_fm_slot *slot, unsigned freq) {
  unsigned det = pfm_dettable[slot->det & 0x3][slot->keycode];
  if (slot->det & 0x4) det = -det;
  freq += det;
  freq &= (1u << 17) - 1;
  int mul = slot->mul << 1;
  if (!mul) mul = 1;
  return (uint32_t)((freq * mul) >> 1);
}

/* Recompute channel c's four slot phase increments from the current freq
   source(s). Call after ANY write that changes blk/fnum/det/mul/keycode/CH3. */
static void opna_fm_chan_calc_phase_inc(struct opna_fm *fm, int c) {
  struct opna_fm_channel *chan = &fm->channel[c];
  if (c == 2 && fm->ch3.mode != CH3_MODE_NORMAL) {
    chan->slot[0].phase_inc = slot_phase_inc(&chan->slot[0], blkfnum2freq(fm->ch3.blk[0], fm->ch3.fnum[0]));
    chan->slot[1].phase_inc = slot_phase_inc(&chan->slot[1], blkfnum2freq(fm->ch3.blk[1], fm->ch3.fnum[1]));
    chan->slot[2].phase_inc = slot_phase_inc(&chan->slot[2], blkfnum2freq(fm->ch3.blk[2], fm->ch3.fnum[2]));
    chan->slot[3].phase_inc = slot_phase_inc(&chan->slot[3], blkfnum2freq(chan->blk, chan->fnum));
  } else {
    unsigned freq = blkfnum2freq(chan->blk, chan->fnum);
    for (int i = 0; i < 4; i++)
      chan->slot[i].phase_inc = slot_phase_inc(&chan->slot[i], freq);
  }
}

/* Per-sample phase advance: just add the precomputed increment (both normal and
   CH3-SE cases — the SE freq source is already baked into phase_inc). */
static void opna_fm_chan_phase(struct opna_fm_channel *chan) {
  chan->slot[0].phase += chan->slot[0].phase_inc;
  chan->slot[1].phase += chan->slot[1].phase_inc;
  chan->slot[2].phase += chan->slot[2].phase_inc;
  chan->slot[3].phase += chan->slot[3].phase_inc;
}

struct fm_frame {
  int16_t data[2];
};

/* One channel's stereo output for the current sample (feedback + 8 algorithms).
   Verified against YM2608/YMF288 in the reference. */
static struct fm_frame opna_fm_chanout(struct opna_fm_channel *chan) {
  int16_t slot0 = chan->slot[0].prevout;
  int16_t slot1 = chan->slot[1].prevout;
  int16_t slot2 = chan->slot[2].prevout;
  int16_t slot3 = chan->slot[3].prevout;
  int16_t fb = chan->fbmem + chan->slot[0].prevout;
  chan->fbmem = slot0;
  if (!chan->fb) fb = 0;
  opna_fm_slotout(&chan->slot[0], fb >> (9 - chan->fb));

  int16_t prev_alg_mem = chan->alg_mem;
  struct fm_frame ret;
  switch (chan->alg) {
  case 0:
    opna_fm_slotout(&chan->slot[1], chan->slot[0].prevout);
    opna_fm_slotout(&chan->slot[2], slot1);
    opna_fm_slotout(&chan->slot[3], slot2);
    ret.data[0] = ret.data[1] = chan->slot[3].prevout >> 1;
    break;
  case 1:
    opna_fm_slotout(&chan->slot[1], 0);
    opna_fm_slotout(&chan->slot[2], prev_alg_mem);
    opna_fm_slotout(&chan->slot[3], slot2);
    chan->alg_mem = chan->slot[0].prevout;
    chan->alg_mem += chan->slot[1].prevout;
    chan->alg_mem &= ~1;
    ret.data[0] = ret.data[1] = chan->slot[3].prevout >> 1;
    break;
  case 2:
    opna_fm_slotout(&chan->slot[1], 0);
    opna_fm_slotout(&chan->slot[2], slot1);
    opna_fm_slotout(&chan->slot[3], slot0 + slot2);
    ret.data[0] = ret.data[1] = chan->slot[3].prevout >> 1;
    break;
  case 3:
    opna_fm_slotout(&chan->slot[1], chan->slot[0].prevout);
    opna_fm_slotout(&chan->slot[2], 0);
    opna_fm_slotout(&chan->slot[3], slot2 + prev_alg_mem);
    chan->alg_mem = slot1;
    ret.data[0] = ret.data[1] = chan->slot[3].prevout >> 1;
    break;
  case 4:
    opna_fm_slotout(&chan->slot[1], slot0);
    opna_fm_slotout(&chan->slot[2], 0);
    opna_fm_slotout(&chan->slot[3], chan->slot[2].prevout);
    ret.data[0] = ret.data[1] = slot3 >> 1;
    ret.data[0] += chan->slot[1].prevout >> 1;
    ret.data[1] += slot1 >> 1;
    break;
  case 5:
    opna_fm_slotout(&chan->slot[1], slot0);
    opna_fm_slotout(&chan->slot[2], slot0);
    opna_fm_slotout(&chan->slot[3], slot0);
    chan->alg_mem = slot2;
    chan->alg_mem &= ~1;
    ret.data[0] = ret.data[1] = slot3 >> 1;
    ret.data[0] += (chan->slot[1].prevout >> 1) + (slot2 >> 1);
    ret.data[1] += (slot1 >> 1) + (prev_alg_mem >> 1);
    break;
  case 6:
    opna_fm_slotout(&chan->slot[1], slot0);
    opna_fm_slotout(&chan->slot[2], 0);
    opna_fm_slotout(&chan->slot[3], 0);
    chan->alg_mem = slot2;
    chan->alg_mem &= ~1;
    ret.data[0] = ret.data[1] = slot3 >> 1;
    ret.data[0] += (chan->slot[1].prevout >> 1) + (slot2 >> 1);
    ret.data[1] += (slot1 >> 1) + (prev_alg_mem >> 1);
    break;
  case 7:
  default:
    opna_fm_slotout(&chan->slot[1], 0);
    opna_fm_slotout(&chan->slot[2], 0);
    opna_fm_slotout(&chan->slot[3], 0);
    chan->alg_mem = chan->slot[1].prevout + chan->slot[2].prevout;
    chan->alg_mem &= ~1;
    ret.data[0] = ret.data[1] =
        (chan->slot[0].prevout >> 1) + (chan->slot[3].prevout >> 1);
    ret.data[0] += chan->alg_mem >> 1;
    ret.data[1] += prev_alg_mem >> 1;
    ret.data[0] <<= 1;
    ret.data[0] >>= 1;
    ret.data[1] <<= 1;
    ret.data[1] >>= 1;
    break;
  }
  return ret;
}

PFM_HOT static void opna_fm_slot_setrate(struct opna_fm_slot *slot, int status) {
  int r;
  switch (status) {
  case PFM_ENV_ATTACK: r = slot->ar; break;
  case PFM_ENV_DECAY: r = slot->dr; break;
  case PFM_ENV_SUSTAIN: r = slot->sr; break;
  case PFM_ENV_RELEASE: r = (slot->rr * 2 + 1); break;
  default: return;
  }
  if (!r) {
    slot->rate_selector = 0;
    slot->rate_mul = 0;
    slot->rate_shifter = 0;
    return;
  }
  int rate = 2 * r + (slot->keycode >> (3 - slot->ks));
  if (rate > 63) rate = 63;
  if (status == PFM_ENV_ATTACK && rate >= 62) rate += 4;
  int rate_shifter = 11 - (rate >> 2);
  if (rate_shifter < 0) {
    slot->rate_selector = (rate & 0x3) + 4;
    slot->rate_mul = 1 << (-rate_shifter - 1);
    slot->rate_shifter = 0;
  } else {
    slot->rate_selector = rate & 0x3;
    slot->rate_mul = 1;
    slot->rate_shifter = rate_shifter;
  }
}

PFM_HOT static void opna_fm_slot_env(struct opna_fm_slot *slot) {
  int rate_shifter = slot->rate_shifter;
  if ((slot->env_count & ((1 << rate_shifter) - 1)) == ((1 << rate_shifter) - 1)) {
    int rate_index = (slot->env_count >> rate_shifter) & 7;
    int env_inc = pfm_rateinctable[slot->rate_selector][rate_index];
    env_inc *= slot->rate_mul;
    int newenv, sl;
    switch (slot->env_state) {
    case PFM_ENV_ATTACK:
      newenv = slot->env + (((-slot->env - 1) * env_inc) >> 4);
      if (newenv <= 0) {
        slot->env = 0;
        slot->env_state = PFM_ENV_DECAY;
        opna_fm_slot_setrate(slot, PFM_ENV_DECAY);
      } else {
        slot->env = newenv;
      }
      break;
    case PFM_ENV_DECAY:
      slot->env += env_inc;
      sl = slot->sl;
      if (sl == 0xf) sl = 0x1f;
      if (slot->env >= (sl << 5)) {
        slot->env_state = PFM_ENV_SUSTAIN;
        opna_fm_slot_setrate(slot, PFM_ENV_SUSTAIN);
      }
      break;
    case PFM_ENV_SUSTAIN:
      slot->env += env_inc;
      if (slot->env >= PFM_FM_ENV_MAX) slot->env = PFM_FM_ENV_MAX;
      break;
    case PFM_ENV_RELEASE:
      slot->env += env_inc;
      if (slot->env >= PFM_FM_ENV_MAX) {
        slot->env = PFM_FM_ENV_MAX;
        slot->env_state = PFM_ENV_OFF;
      }
      break;
    }
  }
  slot->env_count++;
}

PFM_HOT static void opna_fm_slot_key(struct opna_fm_channel *chan, int slotnum, bool keyon) {
  struct opna_fm_slot *slot = &chan->slot[slotnum];
  if (keyon) {
    if (!slot->keyon) {
      slot->keyon = true;
      slot->env_state = PFM_ENV_ATTACK;
      slot->env_count = 0;
      slot->phase = 0;
      slot->prevout = 0;
      opna_fm_slot_setrate(slot, PFM_ENV_ATTACK);
    }
  } else {
    if ((slot->env_state != PFM_ENV_OFF) && slot->keyon) {
      slot->keyon = false;
      slot->env_state = PFM_ENV_RELEASE;
      opna_fm_slot_setrate(slot, PFM_ENV_RELEASE);
    }
  }
}

/* ---- register write helpers ---- */
static void slot_set_ar(struct opna_fm_slot *s, unsigned ar) {
  s->ar = ar & 0x1f;
  if (s->env_state == PFM_ENV_ATTACK) opna_fm_slot_setrate(s, PFM_ENV_ATTACK);
}
static void slot_set_dr(struct opna_fm_slot *s, unsigned dr) {
  s->dr = dr & 0x1f;
  if (s->env_state == PFM_ENV_DECAY) opna_fm_slot_setrate(s, PFM_ENV_DECAY);
}
static void slot_set_sr(struct opna_fm_slot *s, unsigned sr) {
  s->sr = sr & 0x1f;
  if (s->env_state == PFM_ENV_SUSTAIN) opna_fm_slot_setrate(s, PFM_ENV_SUSTAIN);
}
static void slot_set_rr(struct opna_fm_slot *s, unsigned rr) {
  s->rr = rr & 0xf;
  if (s->env_state == PFM_ENV_RELEASE) opna_fm_slot_setrate(s, PFM_ENV_RELEASE);
}

static void opna_fm_chan_set_blkfnum(struct opna_fm_channel *chan, unsigned blk, unsigned fnum) {
  chan->blk = blk & 0x7;
  chan->fnum = fnum & 0x7ff;
  for (int i = 0; i < 4; i++) {
    chan->slot[i].keycode = blkfnum2keycode(chan->blk, chan->fnum);
    opna_fm_slot_setrate(&chan->slot[i], chan->slot[i].env_state);
  }
}

void opna_fm_writereg(struct opna_fm *fm, unsigned reg, unsigned val) {
  val &= 0xff;
  if (reg > 0x1ff) return;

  switch (reg) {
  case 0x27: {
    unsigned mode = val >> 6;
    if (mode != fm->ch3.mode) {
      fm->ch3.mode = mode;
      for (int c = 0; c < 2; c++) {
        unsigned blk, fnum;
        if (fm->ch3.mode == CH3_MODE_NORMAL) {
          blk = fm->channel[2].blk;
          fnum = fm->channel[2].fnum;
        } else {
          blk = fm->ch3.blk[c];
          fnum = fm->ch3.fnum[c];
        }
        fm->channel[2].slot[c].keycode = blkfnum2keycode(blk, fnum);
        opna_fm_slot_setrate(&fm->channel[2].slot[c],
                             fm->channel[2].slot[c].env_state);
      }
      opna_fm_chan_calc_phase_inc(fm, 2);
    }
    return;
  }
  case 0x28: {
    int c = val & 0x3;
    if (c == 3) return;
    if (val & 0x4) c += 3;
    for (int i = 0; i < 4; i++) {
      bool keyon = val & (1 << (4 + i));
      fm->channel[c].slot[i].keyon_ext = keyon;
      if (!keyon) opna_fm_slot_key(&fm->channel[c], i, false);
    }
    return;
  }
  }

  int c = reg & 0x3;
  if (c == 3) return;
  if (reg & (1 << 8)) c += 3;
  int s = ((reg & (1 << 3)) >> 3) | ((reg & (1 << 2)) >> 1);
  struct opna_fm_channel *chan = &fm->channel[c];
  struct opna_fm_slot *slot = &chan->slot[s];
  switch (reg & 0xf0) {
  case 0x30:
    slot->det = (val >> 4) & 0x7;
    slot->mul = val & 0xf;
    opna_fm_chan_calc_phase_inc(fm, c);
    break;
  case 0x40:
    slot->tl = val & 0x7f;
    break;
  case 0x50:
    slot->ks = (val >> 6) & 0x3;
    slot_set_ar(slot, val & 0x1f);
    break;
  case 0x60:
    slot_set_dr(slot, val & 0x1f);
    break;
  case 0x70:
    slot_set_sr(slot, val & 0x1f);
    break;
  case 0x80:
    slot->sl = (val >> 4) & 0xf;
    slot_set_rr(slot, val & 0xf);
    break;
  case 0xa0: {
    unsigned blk = (fm->blkfnum_h >> 3) & 0x7;
    unsigned fnum = ((fm->blkfnum_h & 0x7) << 8) | (val & 0xff);
    switch (reg & 0xc) {
    case 0x0:
      if (c != 2 || fm->ch3.mode == CH3_MODE_NORMAL) {
        opna_fm_chan_set_blkfnum(chan, blk, fnum);
      } else {
        chan->blk = blk;
        chan->fnum = fnum;
        chan->slot[3].keycode = blkfnum2keycode(blk, fnum);
        opna_fm_slot_setrate(&chan->slot[3], chan->slot[3].env_state);
      }
      opna_fm_chan_calc_phase_inc(fm, c);
      break;
    case 0x8:
      c = (c + 2) % 3;
      fm->ch3.blk[c] = blk;
      fm->ch3.fnum[c] = fnum;
      if (fm->ch3.mode != CH3_MODE_NORMAL) {
        fm->channel[2].slot[c].keycode = blkfnum2keycode(blk, fnum);
        opna_fm_slot_setrate(&fm->channel[2].slot[c],
                             fm->channel[2].slot[c].env_state);
      }
      opna_fm_chan_calc_phase_inc(fm, 2);
      break;
    case 0x4:
    case 0xc:
      fm->blkfnum_h = val & 0x3f;
      break;
    }
    break;
  }
  case 0xb0:
    switch (reg & 0xc) {
    case 0x0:
      chan->alg = val & 0x7;
      chan->fb = (val >> 3) & 0x7;
      break;
    case 0x4:
      fm->lselect[c] = val & 0x80;
      fm->rselect[c] = val & 0x40;
      break;
    }
    break;
  }
}

/* A channel whose 4 operators are all in the OFF state AND whose entire signal
   state (prevout, feedback, algorithm memory) is already zero produces exactly
   0 and cannot change its own state (slotout of an OFF operator is 0; keyon
   resets phase). So chanout+phase can be skipped bit-exactly. All four terms are
   load-bearing: an OFF slot may still hold a non-zero prevout for one sample. */
static inline bool chan_is_silent(const struct opna_fm_channel *chan) {
  return chan->fbmem == 0 && chan->alg_mem == 0 &&
         chan->slot[0].env_state == PFM_ENV_OFF && chan->slot[0].prevout == 0 &&
         chan->slot[1].env_state == PFM_ENV_OFF && chan->slot[1].prevout == 0 &&
         chan->slot[2].env_state == PFM_ENV_OFF && chan->slot[2].prevout == 0 &&
         chan->slot[3].env_state == PFM_ENV_OFF && chan->slot[3].prevout == 0;
}

PFM_HOT void opna_fm_mix(struct opna_fm *fm, int16_t *buf, unsigned samples) {
  for (unsigned i = 0; i < samples; i++) {
    if (!fm->env_div3) {
      for (int c = 0; c < 6; c++) {
        for (int s = 0; s < 4; s++) {
          if (fm->channel[c].slot[s].keyon_ext) {
            opna_fm_slot_key(&fm->channel[c], s, true);
            opna_fm_slot_env(&fm->channel[c].slot[s]);
          }
        }
      }
    }

    int32_t lo = buf[i * 2 + 0];
    int32_t ro = buf[i * 2 + 1];

    for (int c = 0; c < 6; c++) {
      if (chan_is_silent(&fm->channel[c])) continue; /* contributes 0, no state change */
      struct fm_frame o = opna_fm_chanout(&fm->channel[c]);
      opna_fm_chan_phase(&fm->channel[c]);
      if (fm->mask & (1 << c)) continue;
      if (fm->lselect[c]) lo += o.data[1];
      if (fm->rselect[c]) ro += o.data[0];
    }

    buf[i * 2 + 0] = (int16_t)pfm_clamp16(lo);
    buf[i * 2 + 1] = (int16_t)pfm_clamp16(ro);

    if (!fm->env_div3) {
      for (int c = 0; c < 6; c++) {
        for (int s = 0; s < 4; s++) {
          if (fm->channel[c].slot[s].keyon_ext) {
            fm->channel[c].slot[s].keyon_ext = false;
          } else {
            opna_fm_slot_env(&fm->channel[c].slot[s]);
          }
        }
      }
      fm->env_div3 = 3;
    }
    fm->env_div3--;
  }
}
