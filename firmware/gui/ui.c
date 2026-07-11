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
static volatile uint32_t g_render_frames; /* cumulative frames produced */
#define AUD_HIGH_WATER 768 /* frames: yield to LCD when the ring is this full */
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
  "FM", "SSG", "DRM", "PCM", "SEQ", "OUT",
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

#include <string.h>

static char g_cur_title[80]; /* Shift-JIS title/name shown on the play screen */

/* Static playback chrome: title + compact 2x3 legend + CPU/DR labels. */
static void draw_play_chrome(void) {
  board_lcd_clear(COL_BG);
  gfx_text(2, BAR_H + 4, g_cur_title, COL_TITLE_FG, COL_BG);
  int cy = BAR_H + 20;
  gfx_text(2, cy, "CPU", COL_NUM, COL_BG);
  gfx_text(30, cy, "%", COL_NUM, COL_BG);
  gfx_text(40, cy, "DR", COL_NUM, COL_BG);
  for (int t = 0; t < PFM_PROF_N; t++) {
    int x = 2 + (t % 3) * 43;
    int y = (BAR_H + 34) + (t / 3) * 12;
    board_lcd_fill_rect(x, y, 7, 7, task_col[t]);
    gfx_text(x + 9, y, task_lbl[t], COL_FG, COL_BG);
  }
  draw_vol(g_volume);
  gfx_text(2, H - 10, "UD vol  LR song  Ctap lcd", COL_NUM, COL_BG);
  board_lcd_present();
}

static void toggle_lcd(void) {
  g_lcd_on = !g_lcd_on;
  LCD_LOCK();
  if (g_lcd_on) draw_play_chrome();
  else { board_lcd_clear(COL_BG); gfx_text(2, 40, "LCD off (tap C)", COL_NUM, COL_BG); board_lcd_present(); }
  LCD_UNLOCK();
}

/* Play the already-loaded player singleton; `display` is the title to show.
   Returns 0 = back to menu, -1 = previous song, +1 = next song. */
static int play(const char *display) {
  pfm_player *p = pfm_player_instance();
  strncpy(g_cur_title, display, sizeof(g_cur_title) - 1);
  g_cur_title[sizeof(g_cur_title) - 1] = 0;

  unsigned rate = PFM_MIX_RATE;
  board_audio_open(rate, 0);
  board_audio_set_volume(g_volume);
  for (int t = 0; t < PFM_PROF_N; t++) pfm_prof_cyc[t] = 0;

  int ret = 0;
  int prevb = board_input_poll();
  int c_armed = !(prevb & BTN_CENTER);

#ifdef PFM_RTOS
  g_render_frames = 0;
  LCD_LOCK();
  if (g_lcd_on) draw_play_chrome();
  else { board_lcd_clear(COL_BG); gfx_text(2, 40, "LCD off (tap C)", COL_NUM, COL_BG); board_lcd_present(); }
  LCD_UNLOCK();
  g_playing = 1;
  TickType_t c_press = 0;
#else
  LCD_LOCK(); draw_play_chrome(); LCD_UNLOCK();
  int refresh = 0, c_held = 0;
  uint64_t win_frames = 0;
  uint32_t cpu_hz = board_cpu_hz(), last_drops = 0, last_cons = 0;
  uint32_t cap = rate * PLAY_SECONDS, done = 0;
#endif

  for (;;) {
#ifdef PFM_RTOS
    if (board_audio_ring_fill() >= (int)AUD_HIGH_WATER) {
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
    if (b & BTN_CENTER) {
      if (c_armed) {
        if (!c_press) c_press = xTaskGetTickCount();
        else if ((xTaskGetTickCount() - c_press) >= pdMS_TO_TICKS(400)) { ret = 0; break; }
      }
    } else {
      if (c_armed && (prevb & BTN_CENTER) && c_press &&
          (xTaskGetTickCount() - c_press) < pdMS_TO_TICKS(400))
        toggle_lcd();
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
      draw_cpu_bar(snap, budget);
      char nb[8]; int cy = BAR_H + 20;
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
#endif
  }
#ifdef PFM_RTOS
  g_playing = 0;
#endif
  board_audio_close();
  return ret;
}

#ifdef PFM_RTOS
/* Lowest-priority meter task (see header). */
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
      int cy = BAR_H + 20;
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

/* ---------------- flash-embedded fallback browser ---------------- */
static int flash_play(int sel) {
  int len = board_storage_load(sel, g_songbuf, sizeof(g_songbuf));
  if (len <= 0) return 0;
  pfm_player *p = pfm_player_instance();
  pfm_player_init(p);
  if (!pfm_player_load(p, g_songbuf, (size_t)len)) return 0;
  const char *t = pfm_player_get_title(p);
  return play((t && t[0]) ? t : board_storage_name(sel));
}

static void ui_run_flash(void) {
  int n = board_storage_count();
  if (n <= 0) {
    LCD_LOCK(); board_lcd_clear(COL_BG); gfx_text(4, 4, "no tracks", COL_ERR, COL_BG); board_lcd_present(); LCD_UNLOCK();
    return;
  }
  int sel = 0, top = 0;
  LCD_LOCK(); draw_list(sel, top, n); LCD_UNLOCK();
  for (;;) {
    int ev = board_input_wait();
    if (!ev) break;
    if (ev == BTN_DOWN && sel < n - 1) sel++;
    else if (ev == BTN_UP && sel > 0) sel--;
    else if (ev == BTN_CENTER) {
      int r = flash_play(sel);
      while (r != 0) {
        sel += r; if (sel < 0) sel = n - 1; else if (sel >= n) sel = 0;
        r = flash_play(sel);
      }
    }
    if (sel < top) top = sel;
    if (sel >= top + VISROWS) top = sel - VISROWS + 1;
    LCD_LOCK(); draw_list(sel, top, n); LCD_UNLOCK();
  }
}

/* ---------------- SD-card hierarchical (tree) browser ---------------- */
#ifdef PFM_RTOS
#include "ff.h"
static FATFS g_fatfs;
static char g_path[256] = "/";
#define BR_NAMELEN 48
static char g_names[VISROWS][BR_NAMELEN];
static uint8_t g_isdir[VISROWS];

static int sd_dir_count(void) {
  DIR dir; FILINFO fno; int n = 0;
  if (f_opendir(&dir, g_path) != FR_OK) return 0;
  while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) n++;
  f_closedir(&dir);
  return n;
}
static void sd_dir_window(int top) {
  for (int w = 0; w < VISROWS; w++) g_names[w][0] = 0;
  DIR dir; FILINFO fno; int n = 0;
  if (f_opendir(&dir, g_path) != FR_OK) return;
  while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
    if (n >= top && n < top + VISROWS) {
      int w = n - top;
      strncpy(g_names[w], fno.fname, BR_NAMELEN - 1);
      g_names[w][BR_NAMELEN - 1] = 0;
      g_isdir[w] = (fno.fattrib & AM_DIR) ? 1 : 0;
    }
    if (++n >= top + VISROWS) break;
  }
  f_closedir(&dir);
}
static int sd_entry(int idx, char *name, int *isdir) {
  DIR dir; FILINFO fno; int n = 0, rc = -1;
  if (f_opendir(&dir, g_path) != FR_OK) return -1;
  while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
    if (n == idx) {
      strncpy(name, fno.fname, BR_NAMELEN - 1); name[BR_NAMELEN - 1] = 0;
      *isdir = (fno.fattrib & AM_DIR) ? 1 : 0; rc = 0; break;
    }
    n++;
  }
  f_closedir(&dir);
  return rc;
}
static void path_push(const char *name) {
  int pl = (int)strlen(g_path);
  if (pl && g_path[pl - 1] != '/' && pl < 254) g_path[pl++] = '/';
  for (int i = 0; name[i] && pl < 254; i++) g_path[pl++] = name[i];
  g_path[pl] = 0;
}
static void path_pop(void) {
  char *last = strrchr(g_path, '/');
  if (last && last != g_path) *last = 0;
  else { g_path[0] = '/'; g_path[1] = 0; }
}
static int sd_load_file(const char *name) {
  char full[300];
  int pl = (int)strlen(g_path);
  for (int i = 0; i < pl; i++) full[i] = g_path[i];
  if (pl && full[pl - 1] != '/') full[pl++] = '/';
  for (int i = 0; name[i] && pl < 299; i++) full[pl++] = name[i];
  full[pl] = 0;
  FIL fp;
  if (f_open(&fp, full, FA_READ) != FR_OK) return -1;
  UINT br = 0;
  f_read(&fp, g_songbuf, sizeof(g_songbuf), &br);
  f_close(&fp);
  return (int)br;
}
static int sd_next_file(int idx, int dir, int count) {
  for (int i = 0; i < count; i++) {
    idx += dir; if (idx < 0) idx = count - 1; else if (idx >= count) idx = 0;
    char nm[BR_NAMELEN]; int isd;
    if (sd_entry(idx, nm, &isd) == 0 && !isd) return idx;
  }
  return -1;
}
static int sd_play_index(int idx) {
  char name[BR_NAMELEN]; int isd;
  if (sd_entry(idx, name, &isd) != 0 || isd) return 0;
  int len = sd_load_file(name);
  if (len <= 0) return 0;
  pfm_player *p = pfm_player_instance();
  pfm_player_init(p);
  if (!pfm_player_load(p, g_songbuf, (size_t)len)) {
    LCD_LOCK(); board_lcd_clear(COL_BG); gfx_text(2, 40, "not a PMD file", COL_ERR, COL_BG); board_lcd_present(); LCD_UNLOCK();
    vTaskDelay(pdMS_TO_TICKS(700));
    return 0;
  }
  const char *t = pfm_player_get_title(p);
  return play((t && t[0]) ? t : name);
}
static void draw_browser(int sel, int top, int count) {
  LCD_LOCK();
  board_lcd_clear(COL_BG);
  board_lcd_fill_rect(0, 0, W, TITLE_H, COL_TITLE_BG);
  gfx_text(2, 4, g_path, COL_TITLE_FG, COL_TITLE_BG);
  sd_dir_window(top);
  for (int r = 0; r < VISROWS; r++) {
    int idx = top + r;
    if (idx >= count || !g_names[r][0]) break;
    int y = LIST_Y + r * ROW_H;
    int issel = (idx == sel);
    uint16_t bg = issel ? COL_SEL_BG : COL_BG;
    uint16_t fg = issel ? COL_SEL_FG : (g_isdir[r] ? COL_SUB_FG : COL_FG);
    board_lcd_fill_rect(0, y, W, ROW_H, bg);
    int x = 2;
    if (g_isdir[r]) x = gfx_text(2, y + 1, "/", COL_ACCENT, bg);
    gfx_text(x, y + 1, g_names[r], fg, bg);
  }
  board_lcd_fill_rect(0, H - FOOT_H, W, FOOT_H, COL_TITLE_BG);
  gfx_text(2, H - FOOT_H + 2, "UD sel LR dir C play", COL_TITLE_FG, COL_TITLE_BG);
  board_lcd_present();
  LCD_UNLOCK();
}
static void ui_run_sd(void) {
  int sel = 0, top = 0;
  int count = sd_dir_count();
  draw_browser(sel, top, count);
  for (;;) {
    int ev = board_input_wait();
    if (!ev) break;
    if (ev == BTN_DOWN) { if (sel < count - 1) sel++; }
    else if (ev == BTN_UP) { if (sel > 0) sel--; }
    else if (ev == BTN_LEFT) { path_pop(); sel = 0; top = 0; count = sd_dir_count(); }
    else if (ev == BTN_RIGHT) {
      char name[BR_NAMELEN]; int isd;
      if (sd_entry(sel, name, &isd) == 0 && isd) { path_push(name); sel = 0; top = 0; count = sd_dir_count(); }
    } else if (ev == BTN_CENTER) {
      char name[BR_NAMELEN]; int isd;
      if (sd_entry(sel, name, &isd) == 0) {
        if (isd) { path_push(name); sel = 0; top = 0; count = sd_dir_count(); }
        else {
          int idx = sel;
          for (;;) {
            int r = sd_play_index(idx);
            if (r == 0) break;
            int ni = sd_next_file(idx, r, count);
            if (ni < 0) break;
            idx = ni;
          }
          sel = idx;
        }
      }
    }
    if (sel < top) top = sel;
    if (sel >= top + VISROWS) top = sel - VISROWS + 1;
    draw_browser(sel, top, count);
  }
}
#endif

void ui_run(void) {
#ifdef PFM_RTOS
  static FATFS *fs = &g_fatfs;
  if (f_mount(fs, "", 1) == FR_OK) { ui_run_sd(); return; }
#endif
  ui_run_flash();
}
