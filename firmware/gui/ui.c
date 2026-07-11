#include "ui.h"
#include "gfx.h"
#include "board.h"
#include "ui_strings.h"
#include "pfm/pfm_config.h"
#include "pfm/pfm_prof.h"
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

static uint8_t g_songbuf[22 * 1024]; /* >= SONG_MAX_LEN (21071); rest freed for .ramfunc */
static int16_t g_chunk[512 * 2];
static int g_volume = 5;             /* 0..BOARD_VOL_MAX, persists across songs */
static int g_lcd_on = 1;             /* tap C toggles LCD drawing during playback */

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

/* ---- CPU meter: one distinct colour per profiled task ---- */
#define COL_IDLE rgb565(38, 42, 54)
#define BAR_H 7
/* constant-expression RGB565 (rgb565() is an inline fn, not usable in a static
   initializer) */
#define RGB_C(r, g, b) \
  ((uint16_t)((((r) & 0xf8) << 8) | (((g) & 0xfc) << 3) | ((b) >> 3)))
static const uint16_t task_col[PFM_PROF_N] = {
  RGB_C(255, 70, 70),   /* FM       */
  RGB_C(90, 220, 90),   /* SSG      */
  RGB_C(80, 130, 255),  /* DRUM     */
  RGB_C(240, 210, 60),  /* PCM      */
  RGB_C(230, 90, 220),  /* SEQ      */
  RGB_C(60, 220, 220),  /* RESAMPLE */
};
static const char *const task_lbl[PFM_PROF_N] = {
  "FM", "SSG", "DRUM", "PCM", "SEQ", "OUTPUT",
};

/* Full bar width = 100% of the real-time CPU budget. Draw each task's share as
   a coloured segment left-to-right; the leftover to the right is idle headroom.
   If the tasks overrun 100% the bar saturates and the CPU% reads red. */
static void draw_cpu_bar(const uint32_t *snap, uint64_t budget) {
  int x = 0, over = 0;
  for (int t = 0; t < PFM_PROF_N; t++) {
    int w = budget ? (int)((uint64_t)snap[t] * W / budget) : 0;
    if (x + w > W) { w = W - x; over = 1; }
    if (w > 0) board_lcd_fill_rect(x, 0, w, BAR_H, task_col[t]);
    x += w;
    if (x >= W) { over = 1; break; }
  }
  if (x < W) board_lcd_fill_rect(x, 0, W - x, BAR_H, COL_IDLE);
  if (over) board_lcd_fill_rect(W - 2, 0, 2, BAR_H, COL_ERR);
}

static void draw_vol(int vol) {
  int y = H - 22;
  board_lcd_fill_rect(2, y, W - 4, GFX_CH + 1, COL_BG);
  char b[4];
  u2a(b, vol, 2);
  int x = gfx_text(2, y, "VOL ", COL_NUM, COL_BG);
  gfx_text(x, y, b, COL_ACCENT, COL_BG);
  int bw = W - 44, fw = vol * bw / BOARD_VOL_MAX;
  board_lcd_fill_rect(40, y + 1, bw, 5, COL_SUB_BG);
  board_lcd_fill_rect(40, y + 1, fw, 5, COL_BAR);
}

#define PLAY_LEGEND_Y (BAR_H + 60)

/* (Re)draw the static playback screen chrome (title, legend, volume, hint). */
static void draw_play_chrome(int sel) {
  board_lcd_clear(COL_BG);
  gfx_text(2, BAR_H + 4, UI_NOWPLAYING, COL_TITLE_FG, COL_BG);
  const char *grp = board_storage_group(sel);
  if (grp && grp[0]) gfx_text(2, BAR_H + 16, grp, COL_SUB_FG, COL_BG);
  gfx_text(2, BAR_H + 28, board_storage_name(sel), COL_FG, COL_BG);
  for (int t = 0; t < PFM_PROF_N; t++) {
    int yy = PLAY_LEGEND_Y + t * 12;
    board_lcd_fill_rect(2, yy, 9, 9, task_col[t]);
    gfx_text(14, yy + 1, task_lbl[t], COL_FG, COL_BG);
  }
  draw_vol(g_volume);
  gfx_text(2, H - 10, "UD:vol LR:song Ctap:lcd Chold:back", COL_NUM, COL_BG);
  board_lcd_present();
}

/* Returns 0 = back to menu, -1 = previous song, +1 = next song. */
static int play(int sel) {
  board_lcd_clear(COL_BG);
  gfx_text(2, BAR_H + 4, UI_NOWPLAYING, COL_TITLE_FG, COL_BG);
  const char *grp = board_storage_group(sel);
  if (grp && grp[0]) gfx_text(2, BAR_H + 16, grp, COL_SUB_FG, COL_BG);
  gfx_text(2, BAR_H + 28, board_storage_name(sel), COL_FG, COL_BG);

  int len = board_storage_load(sel, g_songbuf, sizeof(g_songbuf));
  if (len <= 0) {
    gfx_text(2, 80, "load error", COL_ERR, COL_BG);
    board_lcd_present();
    return 0;
  }
  pfm_player *p = pfm_player_instance();
  pfm_player_init(p);
  if (!pfm_player_load(p, g_songbuf, (size_t)len)) {
    gfx_text(2, 80, "not a PMD file", COL_ERR, COL_BG);
    board_lcd_present();
    return 0;
  }

  unsigned rate = PFM_MIX_RATE;
  board_audio_open(rate, 0);
  board_audio_set_volume(g_volume);
  if (g_lcd_on) draw_play_chrome(sel);
  else { board_lcd_clear(COL_BG); gfx_text(2, 40, "LCD off (tap C)", COL_NUM, COL_BG); board_lcd_present(); }

  for (int t = 0; t < PFM_PROF_N; t++) pfm_prof_cyc[t] = 0;
  uint32_t cpu_hz = board_cpu_hz();
  uint64_t win_frames = 0;
  int refresh = 0, ret = 0, c_held = 0;
  int prevb = board_input_poll(); /* current held buttons (e.g. the start-press) */
  int c_armed = !(prevb & BTN_CENTER); /* ignore C until the start-press releases */
#ifdef PFM_SIM
  uint32_t cap = rate * PLAY_SECONDS, done = 0; /* bounded render for the WAV harness */
#endif

  for (;;) {
    unsigned n = 512;
    pfm_player_render(p, g_chunk, n);
    board_audio_write(g_chunk, n);
    win_frames += n;
#ifdef PFM_SIM
    done += n;
    if (done >= cap) break;
#else
    int b = board_input_poll();
    int edge = b & ~prevb; /* new presses only */
    if (edge & BTN_LEFT) { ret = -1; break; }
    else if (edge & BTN_RIGHT) { ret = 1; break; }
    else if (edge & BTN_UP) {
      if (g_volume < BOARD_VOL_MAX) g_volume++;
      board_audio_set_volume(g_volume);
      if (g_lcd_on) draw_vol(g_volume);
    } else if (edge & BTN_DOWN) {
      if (g_volume > 0) g_volume--;
      board_audio_set_volume(g_volume);
      if (g_lcd_on) draw_vol(g_volume);
    }
    /* CENTER: hold ~0.4s -> back to menu; short tap -> toggle LCD drawing */
    if (b & BTN_CENTER) {
      if (c_armed && ++c_held >= 40) { ret = 0; break; }
    } else {
      if (c_armed && (prevb & BTN_CENTER) && c_held > 0 && c_held < 40) {
        g_lcd_on = !g_lcd_on;
        if (g_lcd_on) draw_play_chrome(sel);
        else { board_lcd_clear(COL_BG); gfx_text(2, 40, "LCD off (tap C)", COL_NUM, COL_BG); board_lcd_present(); }
      }
      c_armed = 1;
      c_held = 0;
    }
    prevb = b;
#endif

    if (++refresh >= 8) {
      refresh = 0;
      uint32_t snap[PFM_PROF_N];
      uint64_t sum = 0;
      for (int t = 0; t < PFM_PROF_N; t++) {
        snap[t] = pfm_prof_cyc[t];
        pfm_prof_cyc[t] = 0;
        sum += snap[t];
      }
      uint64_t budget = win_frames * cpu_hz / rate; /* realtime cycles for the window */
      win_frames = 0;
      if (g_lcd_on) {                 /* skip all LCD writes when toggled off */
        draw_cpu_bar(snap, budget);
        char nb[8];
        unsigned pct = budget ? (unsigned)(sum * 100 / budget) : 0;
        int cy = BAR_H + 44;
        board_lcd_fill_rect(2, cy, W - 4, GFX_CH + 2, COL_BG);
        int xx = gfx_text(2, cy, "CPU ", COL_NUM, COL_BG);
        u2a(nb, pct > 999 ? 999 : pct, 3);
        xx = gfx_text(xx, cy, nb, pct > 100 ? COL_ERR : COL_OK, COL_BG);
        xx = gfx_text(xx, cy, "% DROP ", COL_NUM, COL_BG);
        unsigned drops = board_audio_underruns();
        u2a(nb, drops > 9999 ? 9999 : drops, 1);
        gfx_text(xx, cy, nb, drops ? COL_ERR : COL_OK, COL_BG);
        for (int t = 0; t < PFM_PROF_N; t++) {
          unsigned tp = budget ? (unsigned)((uint64_t)snap[t] * 100 / budget) : 0;
          int yy = PLAY_LEGEND_Y + t * 12 + 1;
          u2a(nb, tp > 999 ? 999 : tp, 3);
          board_lcd_fill_rect(84, yy, W - 84, GFX_CH, COL_BG);
          int px = gfx_text(84, yy, nb, task_col[t], COL_BG);
          gfx_text(px, yy, "%", COL_NUM, COL_BG);
        }
        board_lcd_present();
      }
    }
  }
  board_audio_close();
  return ret;
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
      int r = play(sel);
      while (r != 0) {           /* L/R in playback -> prev/next song, keep playing */
        sel += r;
        if (sel < 0) sel = n - 1;
        else if (sel >= n) sel = 0;
        if (sel < top) top = sel;
        if (sel >= top + VISROWS) top = sel - VISROWS + 1;
        r = play(sel);
      }
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
