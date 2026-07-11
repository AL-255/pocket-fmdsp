#ifndef PFM_PLAYER_H_INCLUDED
#define PFM_PLAYER_H_INCLUDED

/*
 * pocket-fmdsp player: binds the PMD sequence driver to the optimized OPNA
 * emulator and renders interleaved-stereo int16 audio at PFM_MIX_RATE.
 *
 * Opaque handle so this public header stays free of the vendored driver types.
 * Allocate pfm_player_sizeof() bytes (static or heap) and pass the pointer in.
 */

#include "pfm/pfm_config.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pfm_player pfm_player;

/* Storage size / alignment for a player instance. */
size_t pfm_player_sizeof(void);
size_t pfm_player_alignof(void);

/* Process-wide singleton in static storage (no malloc) — for embedded use. */
pfm_player *pfm_player_instance(void);

/* Reset a player instance (call before load). */
void pfm_player_init(pfm_player *p);

/* Attach a PMD .M song. `file` (the whole image) must stay valid while playing;
   the driver indexes into it directly (no copy). Returns true on success. */
bool pfm_player_load(pfm_player *p, const uint8_t *file, size_t len);

/* Optional: supply the 8kB YM2608 rhythm ROM to make drums audible. */
void pfm_player_set_drumrom(pfm_player *p, const uint8_t *rom8k);

/* Render `frames` interleaved-stereo int16 frames. */
void pfm_player_render(pfm_player *p, int16_t *buf, size_t frames);

/* How many times the song has looped so far (driver loop counter). */
unsigned pfm_player_loopcount(const pfm_player *p);

/* Embedded song title (PMD #Title), Shift-JIS; "" if none. Valid after load. */
const char *pfm_player_get_title(pfm_player *p);

#ifdef __cplusplus
}
#endif

#endif /* PFM_PLAYER_H_INCLUDED */
