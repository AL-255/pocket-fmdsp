/*
 * PMD .M loader (freestanding, no libc beyond the header helpers).
 * Byte layout mirrors 98fmplayer/fmdriver/fmdriver_pmd.c:pmd_data_init.
 */
#include "pfm/pmd.h"

bool pmd_song_load(struct pmd_song *s, const uint8_t *file, size_t filelen) {
  if (!file || filelen < 0x19 + 1) return false; /* need data[-1] + >=0x18 header */

  s->opm_flag = file[0];
  if (s->opm_flag > 1) return false;

  s->data = file + 1;
  s->datalen = (uint16_t)(filelen - 1);
  const uint8_t *d = s->data;

  /* instrument table present unless the first part pointer is exactly 0x18 */
  if (d[0] != 0x18) {
    if (s->datalen < 0x1a) return false;
    s->tone_ptr = pfm_read16le(&d[0x18]);
    s->tone_included = true;
  } else {
    s->tone_ptr = 0;
    s->tone_included = false;
  }

  for (int i = 0; i < PMD_PART_LOAD_NUM; i++) {
    uint16_t ptr = pfm_read16le(&d[i * 2]);
    if (ptr && ptr >= s->datalen) return false; /* pointer out of range */
    s->part_ptr[i] = ptr;
  }
  s->r_offset = pfm_read16le(&d[0x16]);
  return true;
}

const uint8_t *pmd_song_tone(const struct pmd_song *s, unsigned tonenum) {
  if (!s->tone_included) return NULL;
  for (uint32_t tp = s->tone_ptr;; tp += PMD_TONE_RECORD_LEN) {
    if ((uint32_t)s->datalen < tp + PMD_TONE_RECORD_LEN) return NULL;
    if (s->data[tp] == (uint8_t)tonenum) return &s->data[tp + 1];
  }
}
