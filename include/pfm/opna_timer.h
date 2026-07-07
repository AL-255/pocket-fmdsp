#ifndef PFM_OPNA_TIMER_H_INCLUDED
#define PFM_OPNA_TIMER_H_INCLUDED

/*
 * OPNA Timer A/B emulation + the audio render loop. Ported verbatim (minus
 * oscillo) from 98fmplayer/libopna/opnatimer.c. This is what drives the music
 * driver: opna_timer_mix() slices the requested buffer at each timer-overflow
 * boundary and fires interrupt_cb (the sequencer tick) on the wrap.
 */

#include "pfm/pfm_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*opna_timer_int_cb_t)(void *ptr);
typedef void (*opna_timer_mix_cb_t)(void *ptr, int16_t *buf, unsigned samples);

struct opna;

struct opna_timer {
  struct opna *opna;
  uint8_t status;
  opna_timer_int_cb_t interrupt_cb;
  void *interrupt_userptr;
  opna_timer_mix_cb_t mix_cb;
  void *mix_userptr;
  uint16_t timera;
  uint8_t timerb;
  bool timera_load, timera_enable;
  bool timerb_load, timerb_enable;
  uint16_t timerb_cnt;
};

void opna_timer_reset(struct opna_timer *timer, struct opna *opna);
uint8_t opna_timer_status(const struct opna_timer *timer);
void opna_timer_set_int_callback(struct opna_timer *timer, opna_timer_int_cb_t f, void *u);
void opna_timer_set_mix_callback(struct opna_timer *timer, opna_timer_mix_cb_t f, void *u);
void opna_timer_writereg(struct opna_timer *timer, unsigned reg, unsigned val);
void opna_timer_mix(struct opna_timer *timer, int16_t *buf, unsigned samples);

#ifdef __cplusplus
}
#endif

#endif /* PFM_OPNA_TIMER_H_INCLUDED */
