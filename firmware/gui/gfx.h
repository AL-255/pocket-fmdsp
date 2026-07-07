#ifndef PFM_GFX_H
#define PFM_GFX_H
#include <stdint.h>

/* Shift-JIS-aware text rendering (Misaki 8x8: half-width 4px, full-width 8px).
   Strings are CP932/Shift-JIS byte sequences; ASCII is a subset. */
#define GFX_CH 8   /* cell height in pixels (scale 1) */

/* Draw an SJIS string at integer pixel `scale`. Returns x past the last glyph. */
int gfx_text_s(int x, int y, const char *sjis, uint16_t fg, uint16_t bg, int scale);
/* scale-1 convenience */
int gfx_text(int x, int y, const char *sjis, uint16_t fg, uint16_t bg);
/* Pixel width of an SJIS string at scale 1. */
int gfx_width(const char *sjis);

#endif /* PFM_GFX_H */
