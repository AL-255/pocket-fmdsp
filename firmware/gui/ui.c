#include "ui.h"
#include "gfx.h"
#include "board.h"
#include "ui_strings.h"
#include "pfm/pfm_config.h"
#include "pfm/player.h"

#define W BOARD_LCD_W
#define H BOARD_LCD_H

#define TITLE_H 16
#define SUB_Y 16
#define SUB_H 12
#define LIST_Y 30
#define ROW_H 11
#define VISROWS 10
#define FOOT_H 12

#define PLAY_SECONDS 6

/* palette (RGB565) */
#define COL_BG        rgb565(10, 12, 22)
#define COL_TITLE_BG  rgb565(36, 40, 96)
#define COL_TITLE_FG  rgb565(255, 226, 120)
#define COL_SUB_BG    rgb565(20, 24, 44)
#define COL_SUB_FG    rgb565(120, 200, 255)
#define COL_FG        rgb565(206, 210, 220)
#define COL_NUM       rgb565(120, 128, 150)
#define COL_SEL_BG    rgb565(200, 60, 70)
#define COL_SEL_FG    rgb565(255, 255, 255)
#define COL_ACCENT    rgb565(120, 230, 160)
#define COL_OK        rgb565(120, 230, 160)
#define COL_ERR       rgb565(255, 110, 110)
#define COL_BAR       rgb565(120, 230, 160)

static uint8_t g_songbuf[24 * 1024];
static int16_t g_chunk[1024 * 2];

static void u2a(char *d, unsigned v, int width) { /* right-aligned decimal */
  char t[8];
  int n = 0;
  do { t[n++] = '0' + v % 10; v /= 10; } while (v);
  int i = 0;
  while (i < width - n) d[i++] = ' ';
  while (n) d[i++] = t[--n];
  d[i] = 0;
}

static void draw_list(int sel, int top, int n) {
  board_lcd_clear(COL_BG);

  board_lcd_fill_rect(0, 0, W, TITLE_H, COL_TITLE_BG);
  gfx_text(2, 4, UI_TITLE, COL_TITLE_FG, COL_TITLE_BG);

  const char *grp = board_storage_group(sel);
  board_lcd_fill_rect(0, SUB_Y, W, SUB_H, COL_SUB_BG);
  gfx_text(2, SUB_Y + 2, (grp && grp[0]) ? grp : UI_SUBTITLE, COL_SUB_FG, COL_SUB_BG);

  for (int r = 0; r < VISROWS; r++) {
    int idx = top + r;
    if (idx >= n) break;
    int y = LIST_Y + r * ROW_H;
    int is_sel = (idx == sel);
    uint16_t bg = is_sel ? COL_SEL_BG : COL_BG;
    uint16_t fg = is_sel ? COL_SEL_FG : COL_FG;
    board_lcd_fill_rect(0, y, W, ROW_H, bg);
    char num[8];
    u2a(num, idx + 1, 3);
    gfx_text(2, y + 1, num, is_sel ? COL_SEL_FG : COL_NUM, bg);
    gfx_text(2 + 3 * 4 + 3, y + 1, board_storage_name(idx), fg, bg);
  }

  board_lcd_fill_rect(0, H - FOOT_H, W, FOOT_H, COL_TITLE_BG);
  gfx_text(2, H - FOOT_H + 2, UI_HINT, COL_TITLE_FG, COL_TITLE_BG);
}

static void play(int sel) {
  board_lcd_clear(COL_BG);
  board_lcd_fill_rect(0, 0, W, TITLE_H, COL_TITLE_BG);
  gfx_text(2, 4, UI_NOWPLAYING, COL_TITLE_FG, COL_TITLE_BG);
  const char *grp = board_storage_group(sel);
  if (grp && grp[0]) gfx_text(2, 22, grp, COL_SUB_FG, COL_BG);
  gfx_text_s(2, 36, board_storage_name(sel), COL_FG, COL_BG, 2);
  gfx_text(2, 60, UI_RENDERING, COL_ACCENT, COL_BG);
  board_lcd_present();

  int len = board_storage_load(sel, g_songbuf, sizeof(g_songbuf));
  if (len <= 0) {
    gfx_text(2, 80, "load error", COL_ERR, COL_BG);
    board_lcd_present();
    return;
  }
  pfm_player *p = pfm_player_instance();
  pfm_player_init(p);
  if (!pfm_player_load(p, g_songbuf, (size_t)len)) {
    gfx_text(2, 80, "not a PMD file", COL_ERR, COL_BG);
    board_lcd_present();
    return;
  }

  unsigned rate = PFM_MIX_RATE;
  uint32_t frames = rate * PLAY_SECONDS;
  board_audio_open(rate, frames);

  int peak = 0;
  for (uint32_t done = 0; done < frames;) {
    uint32_t n = frames - done;
    if (n > 1024) n = 1024;
    pfm_player_render(p, g_chunk, n);
    board_audio_write(g_chunk, n);
    for (uint32_t i = 0; i < n * 2; i++) {
      int v = g_chunk[i] < 0 ? -g_chunk[i] : g_chunk[i];
      if (v > peak) peak = v;
    }
    done += n;
  }
  board_audio_close();

  /* now-playing summary: peak VU bar + size/time */
  board_lcd_fill_rect(0, 58, W, GFX_CH + 2, COL_BG); /* clear "rendering..." line */
  gfx_text(2, 60, UI_DONE, COL_OK, COL_BG);
  int barw = (peak * (W - 8)) / 32767;
  board_lcd_fill_rect(4, 80, W - 8, 8, COL_SUB_BG);
  board_lcd_fill_rect(4, 80, barw, 8, COL_BAR);
  char info[24];
  u2a(info, (unsigned)len, 1);
  int x = gfx_text(2, 96, "size ", COL_NUM, COL_BG);
  x = gfx_text(x, 96, info, COL_FG, COL_BG);
  gfx_text(x, 96, " bytes", COL_NUM, COL_BG);
  gfx_text(2, 108, "audio -> WAV (semihosting)", COL_NUM, COL_BG);
  board_lcd_present();
}

void ui_run(void) {
  int n = board_storage_count();
  if (n <= 0) {
    board_lcd_clear(COL_BG);
    gfx_text(4, 4, "no tracks found", COL_ERR, COL_BG);
    board_lcd_present();
    return;
  }
  int sel = 0, top = 0;
  draw_list(sel, top, n);
  board_lcd_present();

  for (;;) {
    int ev = board_input_wait();
    if (!ev) break; /* sim: input script exhausted -> exit */
    if (ev == BTN_DOWN && sel < n - 1) sel++;
    else if (ev == BTN_UP && sel > 0) sel--;
    else if (ev == BTN_RIGHT) { sel += VISROWS; if (sel > n - 1) sel = n - 1; }
    else if (ev == BTN_LEFT) { sel -= VISROWS; if (sel < 0) sel = 0; }
    else if (ev == BTN_CENTER) {
      play(sel);
      draw_list(sel, top, n);
      board_lcd_present();
      continue;
    }
    if (sel < top) top = sel;
    if (sel >= top + VISROWS) top = sel - VISROWS + 1;
    draw_list(sel, top, n);
    board_lcd_present();
  }
}
