#ifndef PFM_PMD_H_INCLUDED
#define PFM_PMD_H_INCLUDED

/*
 * PMD ".M" song container + loader. This parses the file header (pointer table,
 * rhythm/instrument bases) and gives random access into the resident file image.
 * The tick-driven sequencer that consumes this is built on top (coming next).
 *
 * Parsing follows 98fmplayer/fmdriver/fmdriver_pmd.c (pmd_load / pmd_data_init /
 * pmd_get_toneptr) — the byte layout must match or .M files desync.
 */

#include "pfm/pfm_config.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  PMD_PART_FM_1, PMD_PART_FM_2, PMD_PART_FM_3,
  PMD_PART_FM_4, PMD_PART_FM_5, PMD_PART_FM_6,
  PMD_PART_SSG_1, PMD_PART_SSG_2, PMD_PART_SSG_3,
  PMD_PART_ADPCM, PMD_PART_RHYTHM,
  PMD_PART_LOAD_NUM /* = 11 parts stored in the file pointer table */
};

/* Size of one FM instrument record in the file: tonenum + 25 param bytes. */
#define PMD_TONE_RECORD_LEN 0x1a
#define PMD_TONE_PARAM_LEN 25

struct pmd_song {
  const uint8_t *data;   /* = file + 1 (the driver "base"); must stay resident */
  uint16_t datalen;      /* = filelen - 1 */
  uint8_t opm_flag;      /* file[0] */
  bool tone_included;
  uint16_t tone_ptr;     /* FM instrument table base (offset into data) */
  uint16_t r_offset;     /* rhythm pattern-pointer table base */
  uint16_t part_ptr[PMD_PART_LOAD_NUM];
};

/* Parse `file` (the whole .M image, `filelen` bytes). Returns true on success.
   `file` must remain valid for the lifetime of the song. */
bool pmd_song_load(struct pmd_song *s, const uint8_t *file, size_t filelen);

/* Return a pointer to the 25 parameter bytes of instrument `tonenum`, or NULL. */
const uint8_t *pmd_song_tone(const struct pmd_song *s, unsigned tonenum);

#ifdef __cplusplus
}
#endif

#endif /* PFM_PMD_H_INCLUDED */
