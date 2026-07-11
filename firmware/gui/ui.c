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

#ifdef PFM_RTOS
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
static SemaphoreHandle_t g_lcd_mtx;
static StaticSemaphore_t g_lcd_mtx_buf;
static volatile int g_playing;          /* 1 while a song is rendering */
static volatile int g_cur_sel;          /* song index being played */
static volatile uint32_t g_render_frames; /* cumulative frames produced */
#define AUD_HIGH_WATER 1536 /* frames: yield to LCD when the ring is this full */
#define LCD_LOCK()   xSemaphoreTake(g_lcd_mtx, portMAX_DELAY)
#define LCD_UNLOCK() xSemaphoreGive(g_lcd_mtx)
void ui_init(void) { g_lcd_mtx = xSemaphoreCreateMutexStatic(&g_lcd_mtx_buf); }
#else
#define LCD_LOCK()   ((void)0)
#define LCD_UNLOCK() ((void)0)
void ui_init(void) {}
void ui_lcd_task(void) {}
#endif

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
  { /* static labels; only the two numbers are redrawn each meter tick */
    int cy = BAR_H + 44;
    gfx_text(2, cy, "CPU", COL_NUM, COL_BG);
    gfx_text(30, cy, "%", COL_NUM, COL_BG);
    gfx_text(40, cy, "DR", COL_NUM, COL_BG);
  }
  for (int t = 0; t < PFM_PROF_N; t++) {
    int yy = PLAY_LEGEND_Y + t * 12;
    board_lcd_fill_rect(2, yy, 9, 9, task_col[t]);
    gfx_text(14, yy + 1, task_lbl[t], COL_FG, COL_BG);
  }
  draw_vol(g_volume);
  gfx_text(2, H - 10, "UD:vol LR:song Ctap:lcd Chold:back", COL_NUM, COL_BG);
  board_lcd_present();
}

/* Toggle the LCD-drawing flag and repaint (or blank) the screen. */
static void toggle_lcd(int sel) {
  g_lcd_on = !g_lcd_on;
  LCD_LOCK();
  if (g_lcd_on) draw_play_chrome(sel);
  else {
    board_lcd_clear(COL_BG);
    gfx_text(2, 40, "LCD off (tap C)", COL_NUM, COL_BG);
    board_lcd_present();
  }
  LCD_UNLOCK();
}

/* Returns 0 = back to menu, -1 = previous song, +1 = next song. */
static int play(int sel) {
  LCD_LOCK();
  board_lcd_clear(COL_BG);
  gfx_text(2, BAR_H + 4, UI_NOWPLAYING, COL_TITLE_FG, COL_BG);
  gfx_text(2, BAR_H + 28, board_storage_name(sel), COL_FG, COL_BG);
  LCD_UNLOCK();

  int len = board_storage_load(sel, g_songbuf, sizeof(g_songbuf));
  if (len <= 0) {
    LCD_LOCK(); gfx_text(2, 80, "load error", COL_ERR, COL_BG); board_lcd_present(); LCD_UNLOCK();
    return 0;
  }
  pfm_player *p = pfm_player_instance();
  pfm_player_init(p);
  if (!pfm_player_load(p, g_songbuf, (size_t)len)) {
    LCD_LOCK(); gfx_text(2, 80, "not a PMD file", COL_ERR, COL_BG); board_lcd_present(); LCD_UNLOCK();
    return 0;
  }

  unsigned rate = PFM_MIX_RATE;
  board_audio_open(rate, 0);
  board_audio_set_volume(g_volume);
  for (int t = 0; t < PFM_PROF_N; t++) pfm_prof_cyc[t] = 0;

  int ret = 0;
  int prevb = board_input_poll();
  int c_armed = !(prevb & BTN_CENTER); /* ignore C until the start-press releases */

#ifdef PFM_RTOS
  g_cur_sel = sel;
  g_render_frames = 0;
  LCD_LOCK();
  if (g_lcd_on) draw_play_chrome(sel);
  else { board_lcd_clear(COL_BG); gfx_text(2, 40, "LCD off (tap C)", COL_NUM, COL_BG); board_lcd_present(); }
  LCD_UNLOCK();
  g_playing = 1;
  TickType_t c_press = 0;
#else
  LCD_LOCK(); draw_play_chrome(sel); LCD_UNLOCK();
  int refresh = 0, c_held = 0;
  uint64_t win_frames = 0;
  uint32_t cpu_hz = board_cpu_hz(), last_drops = 0, last_cons = 0;
  uint32_t cap = rate * PLAY_SECONDS, done = 0;
#endif

  for (;;) {
#ifdef PFM_RTOS
    /* audio task: render only when the ring needs it, else yield to LCD/idle.
       The scheduler lets this task preempt a mid-flight LCD draw, so drawing
       can never starve the codec. */
    if (board_audio_ring_fill() >= (int)(AUD_HIGH_WATER)) {
      vTaskDelay(1);
    } else {
      pfm_player_render(p, g_chunk, 512);
      board_audio_write(g_chunk, 512);
      g_render_frames += 512;
    }
#else
    pfm_player_render(p, g_chunk, 512);
    board_audio_write(g_chunk, 512);
    win_frames += 512;
    done += 512;
    if (done >= cap) break;
#endif

    int b = board_input_poll();
    int edge = b & ~prevb;
    if (edge & BTN_LEFT) { ret = -1; break; }
    else if (edge & BTN_RIGHT) { ret = 1; break; }
    else if (edge & BTN_UP) {
      if (g_volume < BOARD_VOL_MAX) g_volume++;
      board_audio_set_volume(g_volume);
      if (g_lcd_on) { LCD_LOCK(); draw_vol(g_volume); LCD_UNLOCK(); }
    } else if (edge & BTN_DOWN) {
      if (g_volume > 0) g_volume--;
      board_audio_set_volume(g_volume);
      if (g_lcd_on) { LCD_LOCK(); draw_vol(g_volume); LCD_UNLOCK(); }
    }
#ifdef PFM_RTOS
    /* CENTER: hold ~0.4s -> back to menu; short tap -> toggle LCD */
    if (b & BTN_CENTER) {
      if (c_armed) {
        if (!c_press) c_press = xTaskGetTickCount();
        else if ((xTaskGetTickCount() - c_press) >= pdMS_TO_TICKS(400)) { ret = 0; break; }
      }
    } else {
      if (c_armed && (prevb & BTN_CENTER) && c_press &&
          (xTaskGetTickCount() - c_press) < pdMS_TO_TICKS(400))
        toggle_lcd(sel);
      c_armed = 1;
      c_press = 0;
    }
#else
    (void)c_armed;
#endif
    prevb = b;

#ifndef PFM_RTOS
    if (++refresh >= 8) {
      refresh = 0;
      uint32_t snap[PFM_PROF_N];
      uint64_t sum = 0;
      for (int t = 0; t < PFM_PROF_N; t++) { snap[t] = pfm_prof_cyc[t]; pfm_prof_cyc[t] = 0; sum += snap[t]; }
      uint64_t budget = win_frames * cpu_hz / rate;
      win_frames = 0;
      uint32_t drops = board_audio_underruns(), cons = board_audio_consumed_frames();
      unsigned drpct = (cons > last_cons) ? (unsigned)((uint64_t)(drops - last_drops) * 100u / (cons - last_cons)) : 0;
      last_drops = drops; last_cons = cons;
      if (g_lcd_on) {
        draw_cpu_bar(snap, budget);
        char nb[8]; int cy = BAR_H + 44;
        unsigned pct = budget ? (unsigned)(sum * 100 / budget) : 0;
        u2a(nb, pct > 999 ? 999 : pct, 3);
        board_lcd_fill_rect(16, cy, 13, GFX_CH, COL_BG);
        gfx_text(16, cy, nb, pct > 100 ? COL_ERR : COL_OK, COL_BG);
        u2a(nb, drpct > 99 ? 99 : drpct, 1);
        board_lcd_fill_rect(50, cy, 20, GFX_CH, COL_BG);
        int dx = gfx_text(50, cy, nb, drpct ? COL_ERR : COL_OK, COL_BG);
        gfx_text(dx, cy, "%", COL_NUM, COL_BG);
        board_lcd_present();
      }
    }
#endif
  }
#ifdef PFM_RTOS
  g_playing = 0;
#endif
  board_audio_close();
  return ret;
}

#ifdef PFM_RTOS
/* Lowest-priority task: paint the playback meter. Runs only in the slices where
   the audio task is ahead and has yielded; the audio task preempts it whenever
   the ring needs refilling, so the display simply drops frames under load. */
void ui_lcd_task(void) {
  uint32_t last_frames = 0, last_drops = 0, last_cons = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(60));
    if (!g_playing || !g_lcd_on) {
      last_frames = g_render_frames;
      last_drops = board_audio_underruns();
      last_cons = board_audio_consumed_frames();
      continue;
    }
    uint32_t snap[PFM_PROF_N];
    uint64_t sum = 0;
    for (int t = 0; t < PFM_PROF_N; t++) { snap[t] = pfm_prof_cyc[t]; pfm_prof_cyc[t] = 0; sum += snap[t]; }
    uint32_t frames = g_render_frames, df = frames - last_frames;
    last_frames = frames;
    uint64_t budget = (uint64_t)df * board_cpu_hz() / PFM_MIX_RATE;
    uint32_t drops = board_audio_underruns(), cons = board_audio_consumed_frames();
    unsigned drpct = (cons > last_cons) ? (unsigned)((uint64_t)(drops - last_drops) * 100u / (cons - last_cons)) : 0;
    last_drops = drops;
    last_cons = cons;
    LCD_LOCK();
    if (g_playing && g_lcd_on) {
      draw_cpu_bar(snap, budget);
      char nb[8];
      int cy = BAR_H + 44;
      unsigned pct = budget ? (unsigned)(sum * 100 / budget) : 0;
      u2a(nb, pct > 999 ? 999 : pct, 3);
      board_lcd_fill_rect(16, cy, 13, GFX_CH, COL_BG);
      gfx_text(16, cy, nb, pct > 100 ? COL_ERR : COL_OK, COL_BG);
      u2a(nb, drpct > 99 ? 99 : drpct, 1);
      board_lcd_fill_rect(50, cy, 20, GFX_CH, COL_BG);
      int dx = gfx_text(50, cy, nb, drpct ? COL_ERR : COL_OK, COL_BG);
      gfx_text(dx, cy, "%", COL_NUM, COL_BG);
      board_lcd_present();
    }
    LCD_UNLOCK();
  }
}
#endif

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
