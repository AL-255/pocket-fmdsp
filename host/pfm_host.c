/*
 * pocket-fmdsp host harness / simulator.
 *
 * Usage:
 *   pfm selftest [outdir]     program the OPNA directly (no driver) and render
 *                             FM/SSG test tones to WAV, with validation.
 *   pfm info <song.M>         parse a PMD .M header and dump its structure.
 *   pfm render <song.M> <out.wav> [seconds]   (driver — coming next)
 *
 * The self-test proves the reimplemented OPNA synth end-to-end before the PMD
 * sequencer exists.
 */
#include "pfm/opna.h"
#include "pfm/player.h"
#include "wav.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- analysis helpers (host-only, double is fine here) ---- */

static double zero_cross_freq(const int16_t *st, size_t frames, unsigned sr) {
  /* count rising zero-crossings on the left channel with a small hysteresis */
  int state = 0;
  size_t crossings = 0;
  for (size_t i = 0; i < frames; i++) {
    int v = st[i * 2];
    if (state <= 0 && v > 512) { crossings++; state = 1; }
    else if (state >= 0 && v < -512) { state = -1; }
  }
  return (double)crossings * sr / (double)frames;
}

static double peak_l(const int16_t *st, size_t frames) {
  int p = 0;
  for (size_t i = 0; i < frames; i++) {
    int v = st[i * 2];
    if (v < 0) v = -v;
    if (v > p) p = v;
  }
  return p;
}

static double rms_l(const int16_t *st, size_t frames) {
  double acc = 0;
  for (size_t i = 0; i < frames; i++) {
    double v = st[i * 2];
    acc += v * v;
  }
  return sqrt(acc / (double)frames);
}

/* Normalised Goertzel magnitude at `freq` on the left channel. */
static double goertzel(const int16_t *st, size_t frames, unsigned sr, double freq) {
  double w = 2.0 * M_PI * freq / sr;
  double coeff = 2.0 * cos(w);
  double s0 = 0, s1 = 0, s2 = 0;
  for (size_t i = 0; i < frames; i++) {
    s0 = st[i * 2] + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
  return sqrt(power) / frames;
}

/* ---- self-test tone programming ---- */

static void fm_program_note(struct opna *o) {
  /* FM ch0: algorithm 7 (additive), slot0 audible carrier, slots1-3 silent.
     ~440 Hz (blk=4, fnum=0x40f, mul=1). */
  opna_writereg(o, 0x27, 0x00); /* CH3 normal mode */
  /* per-operator: reg offsets 0x30/34/38/3c map to physical slots S1,S3,S2,S4 */
  for (int off = 0; off < 16; off += 4) {
    opna_writereg(o, 0x30 + off, 0x01);           /* DT=0, MUL=1 */
    opna_writereg(o, 0x40 + off, off == 0 ? 0 : 127); /* TL: slot0 loud, rest silent */
    opna_writereg(o, 0x50 + off, 0x1f);           /* KS=0, AR=31 */
    opna_writereg(o, 0x60 + off, 0x00);           /* DR=0 */
    opna_writereg(o, 0x70 + off, 0x00);           /* SR=0 */
    opna_writereg(o, 0x80 + off, 0x07);           /* SL=0, RR=7 */
  }
  opna_writereg(o, 0xb0, 0x07); /* FB=0, ALG=7 */
  opna_writereg(o, 0xb4, 0xc0); /* pan L+R */
  opna_writereg(o, 0xa4, 0x24); /* blk=4, fnum hi=0x4 */
  opna_writereg(o, 0xa0, 0x0f); /* fnum lo=0x0f -> fnum=0x40f */
  opna_writereg(o, 0x28, 0xf0); /* key on ch0, all 4 slots */
}

static void ssg_program_note(struct opna *o) {
  /* SSG ch A: ~440 Hz square, max volume, noise off. */
  opna_writereg(o, 0x00, 0x1c); /* tone A period lo */
  opna_writereg(o, 0x01, 0x01); /* tone A period hi -> 0x11c = 284 */
  opna_writereg(o, 0x07, 0x38); /* mixer: tones on (bits0-2=0), noise off (bits3-5=1) */
  opna_writereg(o, 0x08, 0x0f); /* ch A volume max, no envelope */
}

static int render_and_report(const char *label, const char *path,
                             void (*program)(struct opna *), unsigned mask,
                             double expect_hz, const uint8_t *drumrom) {
  static struct opna opna; /* static: keep off the stack (large) */
  opna_reset(&opna);
  if (drumrom) opna_drum_set_rom(&opna.drum, drumrom);
  program(&opna);
  opna_set_mask(&opna, mask);

  const unsigned sr = PFM_MIX_RATE;
  const size_t frames = sr; /* 1 second */
  int16_t *buf = malloc(frames * 2 * sizeof(int16_t));
  if (!buf) { fprintf(stderr, "oom\n"); return 1; }

  /* render in blocks to exercise the block API */
  size_t done = 0;
  while (done < frames) {
    size_t n = frames - done;
    if (n > 1024) n = 1024;
    opna_mix(&opna, buf + done * 2, (unsigned)n);
    done += n;
  }

  double pk = peak_l(buf, frames);
  double rms = rms_l(buf, frames);
  double zf = zero_cross_freq(buf, frames, sr);
  double g_expect = expect_hz > 0 ? goertzel(buf, frames, sr, expect_hz) : 0;
  double g_wrong = expect_hz > 0 ? goertzel(buf, frames, sr, expect_hz * 1.5) : 0;

  int rc = wav_write_s16_stereo(path, buf, frames, sr);
  free(buf);

  printf("[%-4s] peak=%.0f rms=%.0f zerocross=%.1fHz", label, pk, rms, zf);
  if (expect_hz > 0)
    printf(" expect=%.1fHz goertzel(exp)=%.1f goertzel(*1.5)=%.1f", expect_hz, g_expect, g_wrong);
  printf(" -> %s\n", path);

  /* validation */
  int fail = 0;
  if (pk < 1000) { printf("       FAIL: output too quiet\n"); fail = 1; }
  if (expect_hz > 0) {
    if (fabs(zf - expect_hz) > expect_hz * 0.1) {
      printf("       FAIL: zero-cross freq %.1f not within 10%% of %.1f\n", zf, expect_hz);
      fail = 1;
    }
    if (g_expect < g_wrong * 2) {
      printf("       FAIL: no clear spectral peak at %.1fHz\n", expect_hz);
      fail = 1;
    }
  }
  if (rc) { printf("       FAIL: could not write %s\n", path); fail = 1; }
  return fail;
}

static int cmd_selftest(const char *outdir) {
  char p[512];
  int fail = 0;
  const double fm_hz = 439.7;  /* computed from the register settings above */
  const double ssg_hz = 439.4;

  snprintf(p, sizeof p, "%s/selftest_fm.wav", outdir);
  fail |= render_and_report("FM", p, fm_program_note, PFM_CHAN_SSG_1 | PFM_CHAN_SSG_2 | PFM_CHAN_SSG_3, fm_hz, NULL);

  snprintf(p, sizeof p, "%s/selftest_ssg.wav", outdir);
  fail |= render_and_report("SSG", p, ssg_program_note,
                            PFM_CHAN_FM_1 | PFM_CHAN_FM_2 | PFM_CHAN_FM_3 | PFM_CHAN_FM_4 | PFM_CHAN_FM_5 | PFM_CHAN_FM_6,
                            ssg_hz, NULL);

  printf(fail ? "\nSELFTEST: FAIL\n" : "\nSELFTEST: PASS\n");
  return fail ? 1 : 0;
}

/* forward decl for the loader command (implemented in host/pfm_info.c) */
int pfm_cmd_info(const char *path);

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

static int cmd_render(const char *inpath, const char *outpath, double seconds,
                      const char *drumrom_path) {
  size_t len = 0;
  uint8_t *file = read_file(inpath, &len); /* must stay resident: driver indexes it */
  if (!file) { fprintf(stderr, "cannot read %s\n", inpath); return 1; }

  uint8_t *drumrom = NULL;
  if (drumrom_path) {
    size_t rlen = 0;
    drumrom = read_file(drumrom_path, &rlen);
    if (!drumrom || rlen < PFM_DRUM_ROM_SIZE)
      fprintf(stderr, "warning: drum rom %s missing/short, drums silent\n", drumrom_path);
  }

  pfm_player *p = malloc(pfm_player_sizeof());
  if (!p) { fprintf(stderr, "oom\n"); return 1; }
  pfm_player_init(p);
  if (drumrom) pfm_player_set_drumrom(p, drumrom);
  if (!pfm_player_load(p, file, len)) {
    fprintf(stderr, "not a valid PMD .M file: %s\n", inpath);
    return 1;
  }

  const unsigned sr = PFM_MIX_RATE;
  size_t frames = (size_t)(seconds * sr);
  int16_t *pcm = malloc(frames * 2 * sizeof(int16_t));
  if (!pcm) { fprintf(stderr, "oom\n"); return 1; }
  pfm_player_render(p, pcm, frames);

  int rc = wav_write_s16_stereo(outpath, pcm, frames, sr);
  printf("render: %s -> %s (%.1fs, %zu frames, loops=%u)\n", inpath, outpath,
         seconds, frames, pfm_player_loopcount(p));
  free(pcm);
  free(p);
  free(file);
  return rc ? 1 : 0;
}

int main(int argc, char **argv) {
  if (argc >= 2 && !strcmp(argv[1], "selftest")) {
    return cmd_selftest(argc >= 3 ? argv[2] : ".");
  }
  if (argc >= 3 && !strcmp(argv[1], "info")) {
    return pfm_cmd_info(argv[2]);
  }
  if (argc >= 4 && !strcmp(argv[1], "render")) {
    double secs = argc >= 5 ? atof(argv[4]) : 8.0;
    const char *rom = argc >= 6 ? argv[5] : NULL;
    return cmd_render(argv[2], argv[3], secs, rom);
  }
  fprintf(stderr,
          "pocket-fmdsp host harness\n"
          "  %s selftest [outdir]                       render FM/SSG test tones\n"
          "  %s info <song.M>                           parse & dump a PMD .M header\n"
          "  %s render <song.M> <out.wav> [secs] [rom]  play a PMD song to WAV\n",
          argv[0], argv[0], argv[0]);
  return 2;
}
