/*
 * OPNA rhythm (drum) block with on-the-fly ADPCM-A decode. Decode math and
 * output/volume law match 98fmplayer/libopna/opnadrum.c exactly; only the
 * decode is lazy (per-voice streaming) instead of a one-shot 105kB expansion.
 */
#include "pfm/opna_drum.h"

static const uint16_t steps[49] = {
  16, 17, 19, 21, 23, 25, 28,
  31, 34, 37, 41, 45, 50, 55,
  60, 66, 73, 80, 88, 97, 107,
  118, 130, 143, 157, 173, 190, 209,
  230, 253, 279, 307, 337, 371, 408,
  449, 494, 544, 598, 658, 724, 796,
  876, 963, 1060, 1166, 1282, 1411, 1552
};
static const int8_t step_inc[8] = { -1, -1, -1, -1, 2, 5, 7, 9 };

/* ROM region table: {start_byte, end_byte, div}. */
static const struct { unsigned start, end, div; } drum_part[6] = {
  {0x0000, 0x01bf, 3}, /* BD  */
  {0x01c0, 0x043f, 3}, /* SD  */
  {0x0440, 0x1b7f, 3}, /* TOP */
  {0x1b80, 0x1cff, 3}, /* HH  */
  {0x1d00, 0x1f7f, 6}, /* TOM */
  {0x1f80, 0x1fff, 6}, /* RIM */
};

void opna_drum_reset(struct opna_drum *drum) {
  const uint8_t *rom = drum->rom;
  for (unsigned i = 0; i < sizeof(*drum); i++) ((uint8_t *)drum)[i] = 0;
  drum->rom = rom;
  for (int d = 0; d < 6; d++) {
    drum->v[d].start = drum_part[d].start << 1;
    drum->v[d].end = drum_part[d].end;
    drum->v[d].div = drum_part[d].div;
  }
}

void opna_drum_set_rom(struct opna_drum *drum, const uint8_t *rom) {
  drum->rom = rom;
}

/* Decode one nibble at voice->addr, advance addr, return the int16 sample. */
static int16_t drum_decode(struct opna_drum *drum, struct opna_drum_voice *v) {
  unsigned data = drum->rom[v->addr >> 1];
  if (!(v->addr & 1)) data >>= 4;
  data &= 0xf;
  int acc_diff = ((((data & 7) << 1) | 1) * steps[v->step]) >> 3;
  if (data & 8) acc_diff = -acc_diff;
  v->acc += acc_diff;
  v->step += step_inc[data & 7];
  if (v->step < 0) v->step = 0;
  if (v->step > 48) v->step = 48;
  v->addr++;
  int out = v->acc & ((1 << 12) - 1);
  if (out >= (1 << 11)) out -= (1 << 12);
  return (int16_t)(out << 4);
}

static void drum_keyon(struct opna_drum *drum, int d) {
  struct opna_drum_voice *v = &drum->v[d];
  v->addr = v->start;
  v->acc = 0;
  v->step = 0;
  v->sub = 0;
  if (!drum->rom || (v->addr >> 1) == v->end) {
    v->playing = false;
    return;
  }
  v->cur = drum_decode(drum, v);
  v->playing = true;
}

void opna_drum_writereg(struct opna_drum *drum, unsigned reg, unsigned val) {
  val &= 0xff;
  switch (reg) {
  case 0x10:
    for (int d = 0; d < 6; d++) {
      if (val & (1 << d)) {
        if (val & 0x80) drum->v[d].playing = false;
        else drum_keyon(drum, d);
      }
    }
    break;
  case 0x11:
    drum->total_level = val & 0x3f;
    break;
  case 0x18: case 0x19: case 0x1a:
  case 0x1b: case 0x1c: case 0x1d: {
    int d = reg - 0x18;
    drum->v[d].left = val & 0x80;
    drum->v[d].right = val & 0x40;
    drum->v[d].level = val & 0x1f;
    break;
  }
  default:
    break;
  }
}

PFM_HOT void opna_drum_mix(struct opna_drum *drum, int16_t *buf, unsigned samples) {
  if (!drum->rom) return;
  for (unsigned i = 0; i < samples; i++) {
    int32_t lo = buf[i * 2 + 0];
    int32_t ro = buf[i * 2 + 1];
    for (int d = 0; d < 6; d++) {
      struct opna_drum_voice *v = &drum->v[d];
      if (!v->playing) continue;
      int co = v->cur >> 4;
      unsigned level = (v->level ^ 0x1f) + (drum->total_level ^ 0x3f);
      co *= 15 - (int)(level & 7);
      co >>= 1 + (level >> 3);
      if (!(drum->mask & (1u << d))) {
        if (v->left) lo += co;
        if (v->right) ro += co;
      }
      if (++v->sub == v->div) {
        v->sub = 0;
        if ((v->addr >> 1) == v->end) v->playing = false;
        else v->cur = drum_decode(drum, v);
      }
    }
    buf[i * 2 + 0] = (int16_t)pfm_clamp16(lo);
    buf[i * 2 + 1] = (int16_t)pfm_clamp16(ro);
  }
}
