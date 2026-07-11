/* OPNA Timer A/B + render loop. Ported from libopna/opnatimer.c (no oscillo). */
#include "pfm/opna_timer.h"
#include "pfm/opna.h"
#include "pfm/pfm_prof.h"

enum {
  TIMERA_BITS = 10,
  TIMERB_SHIFT = 4,
  TIMERB_BITS = 8 + TIMERB_SHIFT,
};

void opna_timer_reset(struct opna_timer *timer, struct opna *opna) {
  for (unsigned i = 0; i < sizeof(*timer); i++) ((uint8_t *)timer)[i] = 0;
  timer->opna = opna;
}

uint8_t opna_timer_status(const struct opna_timer *timer) { return timer->status; }

void opna_timer_set_int_callback(struct opna_timer *timer, opna_timer_int_cb_t f, void *u) {
  timer->interrupt_cb = f;
  timer->interrupt_userptr = u;
}
void opna_timer_set_mix_callback(struct opna_timer *timer, opna_timer_mix_cb_t f, void *u) {
  timer->mix_cb = f;
  timer->mix_userptr = u;
}

void opna_timer_writereg(struct opna_timer *timer, unsigned reg, unsigned val) {
  val &= 0xff;
  opna_writereg(timer->opna, reg, val);
  switch (reg) {
  case 0x24:
    timer->timera &= ~0xff;
    timer->timera |= val;
    break;
  case 0x25:
    timer->timera &= 0xff;
    timer->timera |= ((val & 3) << 8);
    break;
  case 0x26:
    timer->timerb = val;
    timer->timerb_cnt = timer->timerb << TIMERB_SHIFT;
    break;
  case 0x27:
    timer->timera_load = val & (1 << 0);
    timer->timera_enable = val & (1 << 2);
    timer->timerb_load = val & (1 << 1);
    timer->timerb_enable = val & (1 << 3);
    if (val & (1 << 4)) timer->status &= ~(1 << 0);
    if (val & (1 << 5)) timer->status &= ~(1 << 1);
    break;
  }
}

PFM_HOT void opna_timer_mix(struct opna_timer *timer, int16_t *buf, unsigned samples) {
  do {
    unsigned generate_samples = samples;
    if (timer->timerb_enable && timer->timerb_load) {
      unsigned tb = (1 << TIMERB_BITS) - timer->timerb_cnt;
      if (tb < generate_samples) generate_samples = tb;
    }
    if (timer->timera_enable && timer->timera_load) {
      unsigned ta = (1 << TIMERA_BITS) - timer->timera;
      if (ta < generate_samples) generate_samples = ta;
    }
    opna_mix(timer->opna, buf, generate_samples);
    if (timer->mix_cb) {
      uint32_t t = pfm_prof_begin();
      timer->mix_cb(timer->mix_userptr, buf, generate_samples);
      pfm_prof_end(PFM_PROF_PCM, t);
    }
    buf += generate_samples * 2;
    samples -= generate_samples;
    if (timer->timera_load) {
      timer->timera = (timer->timera + generate_samples) & ((1 << TIMERA_BITS) - 1);
      if (!timer->timera && timer->timera_enable) {
        if (!(timer->status & (1 << 0))) {
          timer->status |= (1 << 0);
          if (timer->interrupt_cb) {
            uint32_t t = pfm_prof_begin();
            timer->interrupt_cb(timer->interrupt_userptr);
            pfm_prof_end(PFM_PROF_SEQ, t);
          }
        }
      }
      timer->timera &= (1 << TIMERA_BITS) - 1;
    }
    if (timer->timerb_load) {
      timer->timerb_cnt = (timer->timerb_cnt + generate_samples) & ((1 << TIMERB_BITS) - 1);
      if (!timer->timerb_cnt && timer->timerb_enable) {
        if (!(timer->status & (1 << 1))) {
          timer->status |= (1 << 1);
          if (timer->interrupt_cb) {
            uint32_t t = pfm_prof_begin();
            timer->interrupt_cb(timer->interrupt_userptr);
            pfm_prof_end(PFM_PROF_SEQ, t);
          }
        }
      }
    }
  } while (samples);
}
