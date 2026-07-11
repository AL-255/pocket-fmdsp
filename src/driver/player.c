/*
 * Player glue: wires the vendored PMD driver's chip callbacks to the optimized
 * OPNA emulator, mirroring 98fmplayer's fmplayer_init_work_opna but with our
 * own OPNA (no 256kB ADPCM RAM; ADPCM regs are harmlessly ignored downstream).
 */
#include "pfm/player.h"
#include "pfm/opna.h"
#include "pfm/opna_timer.h"

/* vendored driver headers (-Isrc/driver/vendor) */
#include "fmdriver/fmdriver.h"
#include "fmdriver/fmdriver_pmd.h"
#include "fmdriver/ppz8.h"

#include <string.h>
#include <stdalign.h>

struct pfm_player {
  struct opna opna;
  struct opna_timer timer;
  struct ppz8 ppz8;
  struct fmdriver_work work;
  struct driver_pmd pmd;
};

size_t pfm_player_sizeof(void) { return sizeof(struct pfm_player); }
size_t pfm_player_alignof(void) { return alignof(struct pfm_player); }

static struct pfm_player g_player_instance;
pfm_player *pfm_player_instance(void) { return &g_player_instance; }

static void cb_writereg(struct fmdriver_work *w, unsigned addr, unsigned data) {
  opna_timer_writereg((struct opna_timer *)w->opna, addr, data);
}
static unsigned cb_readreg(struct fmdriver_work *w, unsigned addr) {
  struct opna_timer *t = (struct opna_timer *)w->opna;
  return opna_readreg(t->opna, addr);
}
static uint8_t cb_status(struct fmdriver_work *w, bool a1) {
  uint8_t s = opna_timer_status((struct opna_timer *)w->opna);
  if (!a1) s &= 0x83;
  return s;
}
static void int_cb(void *u) {
  struct fmdriver_work *w = (struct fmdriver_work *)u;
  w->driver_opna_interrupt(w);
}
static bool g_pcm_muted; /* PCM (ADPCM/PPZ) sits outside the OPNA mask */
static void mix_cb(void *u, int16_t *buf, unsigned samples) {
  if (g_pcm_muted) return; /* leave the FM/SSG/drum mix untouched */
  ppz8_mix((struct ppz8 *)u, buf, samples);
}

void pfm_player_init(pfm_player *p) {
  memset(p, 0, sizeof(*p));
  opna_reset(&p->opna);
  opna_timer_reset(&p->timer, &p->opna);
  ppz8_init(&p->ppz8, PFM_MIX_RATE, 0xa000);
  p->work.opna_writereg = cb_writereg;
  p->work.opna_readreg = cb_readreg;
  p->work.opna_status = cb_status;
  p->work.opna = &p->timer;
  p->work.ppz8 = &p->ppz8;
  p->work.ppz8_functbl = &ppz8_functbl;
  opna_timer_set_int_callback(&p->timer, int_cb, &p->work);
  opna_timer_set_mix_callback(&p->timer, mix_cb, &p->ppz8);
}

bool pfm_player_load(pfm_player *p, const uint8_t *file, size_t len) {
  if (len > 0xffff) return false; /* driver datalen is uint16 */
  if (!pmd_load(&p->pmd, (uint8_t *)file, (uint16_t)len)) return false;
  pmd_init(&p->work, &p->pmd);
  return true;
}

void pfm_player_set_drumrom(pfm_player *p, const uint8_t *rom8k) {
  opna_drum_set_rom(&p->opna.drum, rom8k);
}

PFM_HOT void pfm_player_render(pfm_player *p, int16_t *buf, size_t frames) {
  while (frames) {
    unsigned n = frames > 4096 ? 4096 : (unsigned)frames;
    opna_timer_mix(&p->timer, buf, n);
    buf += (size_t)n * 2;
    frames -= n;
  }
}

unsigned pfm_player_loopcount(const pfm_player *p) { return p->work.loop_cnt; }

void pfm_player_set_mute(pfm_player *p, int fm, int ssg, int drum, int pcm) {
  unsigned m = 0;
  if (fm)   m |= 0x3fu;    /* FM 1-6   */
  if (ssg)  m |= 0x1c0u;   /* SSG 1-3  */
  if (drum) m |= 0x7e00u;  /* rhythm   */
  opna_set_mask(&p->opna, m);
  g_pcm_muted = pcm ? true : false;
}

const char *pfm_player_get_title(pfm_player *p) {
  if (p->work.get_comment) {
    const char *t = p->work.get_comment(&p->work, 0); /* line 0 = #Title (PMD) */
    if (t && t[0]) return t;
  }
  return "";
}
