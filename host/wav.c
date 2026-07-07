#include "wav.h"
#include <stdio.h>
#include <string.h>

static void w32(uint8_t *p, uint32_t v) {
  p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static void w16(uint8_t *p, uint16_t v) {
  p[0] = v; p[1] = v >> 8;
}

int wav_write_s16_stereo(const char *path, const int16_t *interleaved,
                         size_t frames, unsigned samplerate) {
  const unsigned channels = 2, bits = 16;
  uint32_t data_bytes = (uint32_t)frames * channels * (bits / 8);
  uint32_t byte_rate = samplerate * channels * (bits / 8);
  uint8_t h[44];
  memcpy(h + 0, "RIFF", 4);
  w32(h + 4, 36 + data_bytes);
  memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4);
  w32(h + 16, 16);
  w16(h + 20, 1); /* PCM */
  w16(h + 22, channels);
  w32(h + 24, samplerate);
  w32(h + 28, byte_rate);
  w16(h + 32, channels * (bits / 8));
  w16(h + 34, bits);
  memcpy(h + 36, "data", 4);
  w32(h + 40, data_bytes);

  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  int ok = fwrite(h, 1, 44, f) == 44 &&
           fwrite(interleaved, sizeof(int16_t), frames * channels, f) ==
               frames * channels;
  fclose(f);
  return ok ? 0 : -1;
}
