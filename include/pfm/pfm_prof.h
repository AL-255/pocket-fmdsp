#ifndef PFM_PROF_H_INCLUDED
#define PFM_PROF_H_INCLUDED
/*
 * Tiny per-task cycle profiler for the on-device CPU meter. The render path
 * calls pfm_prof_begin()/pfm_prof_end() around each sub-task; a platform
 * installs pfm_prof_clock (e.g. the Cortex-M DWT cycle counter). When the
 * pointer is NULL (default, e.g. host unit tests) it is a no-op with no effect
 * on output -- only a predicted-not-taken branch of overhead.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  PFM_PROF_FM,        /* opna_fm_mix   */
  PFM_PROF_SSG,       /* opna_ssg_mix  */
  PFM_PROF_DRUM,      /* opna_drum_mix */
  PFM_PROF_PCM,       /* ppz8 PCM mix  */
  PFM_PROF_SEQ,       /* PMD sequencer (timer interrupt) */
  PFM_PROF_OUTPUT,    /* board copy into the DMA ring    */
  PFM_PROF_SD,        /* SD card file read (sd task)     */
  PFM_PROF_N
};

extern uint32_t pfm_prof_cyc[PFM_PROF_N];   /* accumulated cycles per task */
extern uint32_t (*pfm_prof_clock)(void);    /* NULL = profiling disabled   */

static inline uint32_t pfm_prof_begin(void) {
  return pfm_prof_clock ? pfm_prof_clock() : 0u;
}
static inline void pfm_prof_end(int id, uint32_t t0) {
  if (pfm_prof_clock) pfm_prof_cyc[id] += pfm_prof_clock() - t0;
}

#ifdef __cplusplus
}
#endif
#endif /* PFM_PROF_H_INCLUDED */
