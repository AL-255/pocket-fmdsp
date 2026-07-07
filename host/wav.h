#ifndef PFM_HOST_WAV_H_INCLUDED
#define PFM_HOST_WAV_H_INCLUDED

#include <stdint.h>
#include <stddef.h>

/* Write interleaved-stereo int16 PCM to a canonical 16-bit WAV file.
   Returns 0 on success, -1 on error. */
int wav_write_s16_stereo(const char *path, const int16_t *interleaved,
                         size_t frames, unsigned samplerate);

#endif
