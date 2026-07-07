/* Host-only: parse a PMD .M file and dump its structure (validates the loader). */
#include "pfm/pmd.h"
#include <stdio.h>
#include <stdlib.h>

static const char *part_name[PMD_PART_LOAD_NUM] = {
  "FM1", "FM2", "FM3", "FM4", "FM5", "FM6",
  "SSG1", "SSG2", "SSG3", "ADPCM", "RHYTHM",
};

static uint8_t *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0) { fclose(f); return NULL; }
  uint8_t *buf = malloc(n);
  if (!buf) { fclose(f); return NULL; }
  size_t got = fread(buf, 1, n, f);
  fclose(f);
  if (got != (size_t)n) { free(buf); return NULL; }
  *len = (size_t)n;
  return buf;
}

int pfm_cmd_info(const char *path) {
  size_t len = 0;
  uint8_t *file = read_file(path, &len);
  if (!file) { fprintf(stderr, "cannot read %s\n", path); return 1; }

  struct pmd_song song;
  if (!pmd_song_load(&song, file, len)) {
    fprintf(stderr, "not a valid PMD .M file: %s (len=%zu, flag=0x%02x)\n",
            path, len, len ? file[0] : 0);
    free(file);
    return 1;
  }

  printf("file        : %s\n", path);
  printf("size        : %zu bytes (data=%u)\n", len, song.datalen);
  printf("opm_flag    : 0x%02x\n", song.opm_flag);
  printf("tone table  : %s", song.tone_included ? "yes" : "no");
  if (song.tone_included) printf(" @ 0x%04x", song.tone_ptr);
  printf("\n");
  printf("rhythm tbl  : 0x%04x\n", song.r_offset);
  printf("parts:\n");
  for (int i = 0; i < PMD_PART_LOAD_NUM; i++) {
    uint16_t p = song.part_ptr[i];
    printf("  %-7s ptr=0x%04x", part_name[i], p);
    if (p && p < song.datalen) {
      printf("  first bytes:");
      for (int k = 0; k < 8 && (p + k) < song.datalen; k++)
        printf(" %02x", song.data[p + k]);
    } else if (!p) {
      printf("  (unused)");
    }
    printf("\n");
  }

  if (song.tone_included) {
    printf("instruments :\n");
    int count = 0;
    for (uint32_t tp = song.tone_ptr;
         (uint32_t)song.datalen >= tp + PMD_TONE_RECORD_LEN;
         tp += PMD_TONE_RECORD_LEN) {
      uint8_t num = song.data[tp];
      /* the instrument table is contiguous at the very start; stop when the
         record's tone number stops looking like a monotonic voice id and we
         have already collected some. Heuristic display only. */
      uint8_t fb_alg = song.data[tp + 1 + 0x18];
      printf("  tone %3u : FB=%u ALG=%u\n", num, (fb_alg >> 3) & 7, fb_alg & 7);
      if (++count >= 64) { printf("  ... (truncated)\n"); break; }
      /* the tone table is followed by sequence data; a robust terminator needs
         the driver. For inspection, stop once we pass any part pointer. */
      uint32_t next = tp + PMD_TONE_RECORD_LEN;
      int hit = 0;
      for (int i = 0; i < PMD_PART_LOAD_NUM; i++)
        if (song.part_ptr[i] && next >= song.part_ptr[i] && song.tone_ptr < song.part_ptr[i]) hit = 1;
      if (hit) break;
    }
    printf("  (%d instrument record(s))\n", count);
  }

  free(file);
  return 0;
}
