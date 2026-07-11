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
static volatile int g_song_loaded;      /* 1 while a song is loaded and rendering */
static volatile uint32_t g_render_frames; /* cumulative frames produced */
#define AUD_HIGH_WATER 512 /* frames: yield to LCD above this; a 512-render always
                              fits the 1024 ring below it, so writes never spin */
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

__attribute__((unused))
static void draw_list(int sel, int top, int n) {  /* sim browser only */
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

/* ============================================================================
   Tab UI: three pages (Browser / Playback / Settings). Long-press Center cycles
   pages; short Center is the page action. A single non-blocking super-loop keeps
   the loaded song rendering no matter which page is shown, so switching tabs
   never interrupts the music (audio is this task; the LCD task is lower prio).
   The sim (no FreeRTOS) keeps a simple flat browser + fixed-length preview.
   ============================================================================ */

/* play-screen geometry (128x160, 4px half-width font) */
#define PLAY_TITLE_Y (BAR_H + 2)
#define PLAY_NAME_Y  (BAR_H + 12)
#define PLAY_CPU_Y   (BAR_H + 23)
#define PLAY_LEG_Y   (BAR_H + 35)

#ifdef PFM_RTOS
#include "ff.h"

enum { PG_BROWSER = 0, PG_PLAY = 1, PG_SETTINGS = 2, PG_N = 3 };
static volatile int g_page = PG_BROWSER;

/* --- currently playing song --- */
static char g_cur_title[80];
static char g_cur_name[48];
static uint32_t g_unmute_cons;   /* consumed-frame count at which to lift the swap mute */
static int g_muted_swap;         /* codec muted across a song swap */
static int g_dbg_len;            /* last load: bytes read (>0) or negative error */

/* --- browser (SD only) --- */
static FATFS g_fatfs;
static int g_have_sd;
static int g_mount_rc;           /* f_mount return code (for the debug view) */
static char g_path[256] = "/";
#define BR_NAMELEN 48
static char g_names[VISROWS][BR_NAMELEN];
static uint8_t g_isdir[VISROWS];
static int b_sel, b_top, b_count;
static char g_play_path[256];    /* dir the playing song lives in (for prev/next) */
static int  g_play_idx;

/* --- settings --- */
#define NSET 5
static const char *const set_lbl[NSET] = { "Output", "FM", "SSG", "Drum", "PCM" };
static int st_sel;
static uint8_t g_out_speaker;    /* 0 headphone, 1 loudspeaker */
static uint8_t g_mute[4];        /* FM, SSG, DRUM, PCM */

/* ---------- SD directory helpers ---------- */
static int sd_dir_count(void) {
  DIR dir; FILINFO fno; int n = 0;
  if (f_opendir(&dir, g_path) != FR_OK) return 0;
  while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) n++;
  f_closedir(&dir);
  return n;
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
  FRESULT fr = f_read(&fp, g_songbuf, sizeof(g_songbuf), &br);
  f_close(&fp);
  if (fr != FR_OK) return -1;   /* disk error after 10 retries -> hard fail */
  return (int)br;
}

/* ---------- source layer: SD tree, or flash flat list when no card ---------- */
static int src_count(void) {
  if (g_have_sd) return sd_dir_count();
  return board_storage_count();
}
static int src_entry(int idx, char *name, int *isdir) {
  if (g_have_sd) return sd_entry(idx, name, isdir);
  { const char *n = board_storage_name(idx);
    if (!n) return -1;
    int i = 0; for (; n[i] && i < BR_NAMELEN - 1; i++) name[i] = n[i];
    name[i] = 0; *isdir = 0; return 0; }
}
static int src_load_idx(int idx) {
  if (g_have_sd) {
    char nm[BR_NAMELEN]; int d;
    if (sd_entry(idx, nm, &d) != 0 || d) return -1;
    return sd_load_file(nm);
  }
  return board_storage_load(idx, g_songbuf, sizeof(g_songbuf));
}
static void src_window(int top) {
  for (int w = 0; w < VISROWS; w++) g_names[w][0] = 0;
  int cnt = src_count();
  for (int w = 0; w < VISROWS; w++) {
    int idx = top + w;
    if (idx >= cnt) break;
    int isd = 0;
    src_entry(idx, g_names[w], &isd);
    g_isdir[w] = (uint8_t)isd;
  }
}

/* ---------- playback screen ---------- */
static void draw_meter_numbers(unsigned pct, unsigned drpct) {
  char nb[8];
  u2a(nb, pct > 999 ? 999 : pct, 3);
  board_lcd_fill_rect(16, PLAY_CPU_Y, 14, GFX_CH, COL_BG);
  int x = gfx_text(16, PLAY_CPU_Y, nb, pct > 100 ? COL_ERR : COL_OK, COL_BG);
  gfx_text(x, PLAY_CPU_Y, "%", COL_NUM, COL_BG);
  u2a(nb, drpct > 99 ? 99 : drpct, 2);
  board_lcd_fill_rect(78, PLAY_CPU_Y, 16, GFX_CH, COL_BG);
  x = gfx_text(78, PLAY_CPU_Y, nb, drpct ? COL_ERR : COL_OK, COL_BG);
  gfx_text(x, PLAY_CPU_Y, "%", COL_NUM, COL_BG);
}

/* ---------- debug panel (live SD / pipeline state) ---------- */
#define PLAY_DBG_Y (PLAY_LEG_Y + 26)
static int dbg_app(char *d, int o, const char *lab, int v) {
  while (*lab) d[o++] = *lab++;
  if (v < 0) { d[o++] = '-'; v = -v; }
  char t[12]; int n = 0;
  do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
  while (n) d[o++] = t[--n];
  d[o++] = ' '; d[o] = 0;
  return o;
}
static void draw_dbg(void) {
  char s[48]; int o = 0;
  o = dbg_app(s, o, "L", g_dbg_len);
  o = dbg_app(s, o, "fill", board_audio_ring_fill());
  o = dbg_app(s, o, "E", (int)board_sd_read_errors());
  board_lcd_fill_rect(2, PLAY_DBG_Y, W - 4, GFX_CH, COL_BG);
  gfx_text(2, PLAY_DBG_Y, s, COL_NUM, COL_BG);
  o = 0;
  o = dbg_app(s, o, "sd", g_have_sd);
  o = dbg_app(s, o, "rc", g_mount_rc);
  o = dbg_app(s, o, "ld", g_song_loaded);
  o = dbg_app(s, o, "m", g_muted_swap);
  board_lcd_fill_rect(2, PLAY_DBG_Y + 11, W - 4, GFX_CH, COL_BG);
  gfx_text(2, PLAY_DBG_Y + 11, s, COL_NUM, COL_BG);
}
static void draw_play_chrome(void) {
  board_lcd_clear(COL_BG);
  gfx_text(2, PLAY_TITLE_Y, g_cur_title[0] ? g_cur_title : "(no title)", COL_TITLE_FG, COL_BG);
  gfx_text(2, PLAY_NAME_Y, g_cur_name, COL_SUB_FG, COL_BG);
  gfx_text(2, PLAY_CPU_Y, "CPU", COL_NUM, COL_BG);
  gfx_text(62, PLAY_CPU_Y, "DR", COL_NUM, COL_BG);
  for (int t = 0; t < PFM_PROF_N; t++) {
    int x = 2 + (t % 3) * 42;
    int y = PLAY_LEG_Y + (t / 3) * 12;
    board_lcd_fill_rect(x, y, 7, 7, task_col[t]);
    gfx_text(x + 9, y, task_lbl[t], COL_FG, COL_BG);
  }
  draw_vol(g_volume);
  draw_dbg();
  gfx_text(2, H - 10, "holdC pages UDvol LRsong", COL_NUM, COL_BG);
  board_lcd_present();
}
static void toggle_lcd(void) {
  g_lcd_on = !g_lcd_on;
  LCD_LOCK();
  if (g_lcd_on) draw_play_chrome();
  else { board_lcd_clear(COL_BG); gfx_text(2, 40, "LCD off (tap C)", COL_NUM, COL_BG); board_lcd_present(); }
  LCD_UNLOCK();
}

/* ---------- browser screen ---------- */
static void draw_browser_rows(void) {
  src_window(b_top);
  for (int r = 0; r < VISROWS; r++) {
    int idx = b_top + r;
    int y = LIST_Y + r * ROW_H;
    int sel = (idx == b_sel && idx < b_count);
    uint16_t bg = sel ? COL_SEL_BG : COL_BG;
    board_lcd_fill_rect(0, y, W, ROW_H, bg);  /* in-place: no full clear, no flash */
    if (idx < b_count && g_names[r][0]) {
      uint16_t fg = sel ? COL_SEL_FG : (g_isdir[r] ? COL_SUB_FG : COL_FG);
      int x = 2;
      if (g_isdir[r]) x = gfx_text(2, y + 1, "/", COL_ACCENT, bg);
      gfx_text(x, y + 1, g_names[r], fg, bg);
    }
  }
  board_lcd_present();
}
static void draw_browser_full(void) {
  board_lcd_clear(COL_BG);
  board_lcd_fill_rect(0, 0, W, TITLE_H, COL_TITLE_BG);
  gfx_text(2, 4, g_have_sd ? g_path : "no SD card", COL_TITLE_FG, COL_TITLE_BG);
  if (!g_have_sd) {
    char m[24];
    strcpy(m, "insert SD (mount ");
    u2a(m + strlen(m), (unsigned)(g_mount_rc < 0 ? -g_mount_rc : g_mount_rc), 1);
    int e = (int)strlen(m); m[e] = ')'; m[e + 1] = 0;
    gfx_text(2, LIST_Y + 4, m, COL_ERR, COL_BG);
    gfx_text(2, LIST_Y + 16, "hold C = other pages", COL_NUM, COL_BG);
  }
  board_lcd_fill_rect(0, H - FOOT_H, W, FOOT_H, COL_TITLE_BG);
  gfx_text(2, H - FOOT_H + 2, "UD sel  LR dir  C play", COL_TITLE_FG, COL_TITLE_BG);
  if (g_have_sd) draw_browser_rows();
  else board_lcd_present();
}
static void browse_reload(void) { b_count = src_count(); b_sel = 0; b_top = 0; }

/* ---------- settings screen ---------- */
static void set_value_str(int i, char *out) {
  if (i == 0) strcpy(out, g_out_speaker ? "Speaker" : "Phones");
  else strcpy(out, g_mute[i - 1] ? "Mute" : "On");
}
static void draw_settings_rows(void) {
  for (int r = 0; r < NSET; r++) {
    int y = LIST_Y + r * ROW_H;
    int sel = (r == st_sel);
    uint16_t bg = sel ? COL_SEL_BG : COL_BG;
    uint16_t fg = sel ? COL_SEL_FG : COL_FG;
    board_lcd_fill_rect(0, y, W, ROW_H, bg);
    gfx_text(4, y + 1, set_lbl[r], fg, bg);
    char v[16]; set_value_str(r, v);
    uint16_t vc = sel ? COL_SEL_FG : ((r > 0 && g_mute[r - 1]) ? COL_ERR : COL_ACCENT);
    gfx_text(70, y + 1, v, vc, bg);
  }
  board_lcd_present();
}
static void draw_settings_full(void) {
  board_lcd_clear(COL_BG);
  board_lcd_fill_rect(0, 0, W, TITLE_H, COL_TITLE_BG);
  gfx_text(2, 4, "Settings", COL_TITLE_FG, COL_TITLE_BG);
  board_lcd_fill_rect(0, H - FOOT_H, W, FOOT_H, COL_TITLE_BG);
  gfx_text(2, H - FOOT_H + 2, "UD sel  LR change", COL_TITLE_FG, COL_TITLE_BG);
  draw_settings_rows();
}

/* ---------- audio control ---------- */
static void apply_mute(pfm_player *p) {
  pfm_player_set_mute(p, g_mute[0], g_mute[1], g_mute[2], g_mute[3]);
}
/* Load file entry `idx` of the current browse dir and make it the playing song.
   Mutes the codec across the swap; the super-loop lifts the mute once the old
   ring tail has drained, so there is no transition pop. Returns 1 on success. */
static int start_song(pfm_player *p, int idx) {
  char name[BR_NAMELEN]; int isd;
  if (src_entry(idx, name, &isd) != 0 || isd) return 0;
  strncpy(g_cur_name, name, sizeof(g_cur_name) - 1);
  g_cur_name[sizeof(g_cur_name) - 1] = 0;
  /* Silence the current song before we overwrite its buffer, so the (blocking)
     SD read plays out muted rather than glitching. */
  if (g_song_loaded) { board_audio_mute(1); g_muted_swap = 1; }
  int len = src_load_idx(idx);                 /* SD read: 10x per-block retry + CRC */
  g_dbg_len = len;
  if (len <= 0) goto fail;                      /* card read failed */
  pfm_player_init(p);
  if (!pfm_player_load(p, g_songbuf, (size_t)len)) { g_dbg_len = -99; goto fail; } /* not PMD */
  apply_mute(p);
  const char *t = pfm_player_get_title(p);
  strncpy(g_cur_title, (t && t[0]) ? t : name, sizeof(g_cur_title) - 1);
  g_cur_title[sizeof(g_cur_title) - 1] = 0;
  strcpy(g_play_path, g_path);
  g_play_idx = idx;
  /* Reset the ENTIRE audio pipeline on every song so nothing carries over between
     tracks: first song opens the codec; a switch re-primes the ring/DMA/counters
     (clears the drift the blocking SD read introduces). */
  if (!g_song_loaded) {
    board_audio_open(PFM_MIX_RATE, 0);
    board_audio_set_volume(g_volume);
    board_audio_set_output(g_out_speaker);
    board_audio_mute(1);
  } else {
    board_audio_restart();
  }
  g_muted_swap = 1;
  g_render_frames = 0;
  g_unmute_cons = board_audio_consumed_frames() + 1024; /* unmute once old tail drains */
  g_song_loaded = 1;
  return 1;
fail:
  /* Read/parse failed; g_songbuf is now clobbered so the old song is gone. Stop
     rendering (don't feed the codec a reset, empty player = silence at 0% CPU). */
  g_song_loaded = 0;
  board_audio_mute(0); g_muted_swap = 0;
  strncpy(g_cur_title, "read error", sizeof(g_cur_title) - 1);
  g_cur_title[sizeof(g_cur_title) - 1] = 0;
  return 0;
}
/* prev/next within the *playing* song's directory (independent of browsing). */
static void play_step(pfm_player *p, int dir) {
  if (!g_song_loaded) return;
  char saved[256]; strcpy(saved, g_path); strcpy(g_path, g_play_path);
  int cnt = src_count(), idx = g_play_idx, found = -1;
  for (int i = 0; i < cnt; i++) {
    idx += dir; if (idx < 0) idx = cnt - 1; else if (idx >= cnt) idx = 0;
    char nm[BR_NAMELEN]; int isd;
    if (src_entry(idx, nm, &isd) == 0 && !isd) { found = idx; break; }
  }
  if (found >= 0) start_song(p, found);
  strcpy(g_path, saved);
  if (g_page == PG_PLAY) { LCD_LOCK(); draw_play_chrome(); LCD_UNLOCK(); }
}

/* ---------- page dispatch ---------- */
static void cycle_page(void) {
  g_page = (g_page + 1) % PG_N;
  LCD_LOCK();
  if (g_page == PG_BROWSER) draw_browser_full();
  else if (g_page == PG_PLAY) draw_play_chrome();
  else draw_settings_full();
  LCD_UNLOCK();
}
static void page_center(pfm_player *p) {
  if (g_page == PG_BROWSER) {
    char name[BR_NAMELEN]; int isd;
    if (src_entry(b_sel, name, &isd) != 0) return;
    if (isd) { path_push(name); browse_reload(); LCD_LOCK(); draw_browser_full(); LCD_UNLOCK(); }
    else { start_song(p, b_sel); g_page = PG_PLAY; LCD_LOCK(); draw_play_chrome(); LCD_UNLOCK(); }
  } else if (g_page == PG_PLAY) {
    toggle_lcd();
  } else {                              /* settings: toggle selected item */
    if (st_sel == 0) g_out_speaker = !g_out_speaker;
    else g_mute[st_sel - 1] = !g_mute[st_sel - 1];
    board_audio_set_output(g_out_speaker);
    if (g_song_loaded) apply_mute(p);
    LCD_LOCK(); draw_settings_rows(); LCD_UNLOCK();
  }
}
static void page_nav(pfm_player *p, int edge) {
  if (g_page == PG_BROWSER) {
    int full = 0;
    if (edge & BTN_DOWN) { if (b_sel < b_count - 1) b_sel++; }
    else if (edge & BTN_UP) { if (b_sel > 0) b_sel--; }
    else if (edge & BTN_RIGHT) {       /* descend */
      char name[BR_NAMELEN]; int isd;
      if (g_have_sd && src_entry(b_sel, name, &isd) == 0 && isd) { path_push(name); browse_reload(); full = 1; }
    } else if (edge & BTN_LEFT) {      /* ascend */
      if (g_have_sd) { path_pop(); browse_reload(); full = 1; }
    }
    if (b_sel < b_top) b_top = b_sel;
    if (b_sel >= b_top + VISROWS) b_top = b_sel - VISROWS + 1;
    LCD_LOCK();
    if (full) draw_browser_full(); else draw_browser_rows();
    LCD_UNLOCK();
  } else if (g_page == PG_PLAY) {
    if (edge & (BTN_UP | BTN_DOWN)) {
      if ((edge & BTN_UP) && g_volume < BOARD_VOL_MAX) g_volume++;
      else if ((edge & BTN_DOWN) && g_volume > 0) g_volume--;
      board_audio_set_volume(g_volume);
      if (g_lcd_on) { LCD_LOCK(); draw_vol(g_volume); board_lcd_present(); LCD_UNLOCK(); }
    } else if (edge & BTN_RIGHT) play_step(p, +1);
    else if (edge & BTN_LEFT) play_step(p, -1);
  } else {                              /* settings */
    if (edge & BTN_DOWN) { if (st_sel < NSET - 1) st_sel++; LCD_LOCK(); draw_settings_rows(); LCD_UNLOCK(); }
    else if (edge & BTN_UP) { if (st_sel > 0) st_sel--; LCD_LOCK(); draw_settings_rows(); LCD_UNLOCK(); }
    else if (edge & (BTN_LEFT | BTN_RIGHT)) {
      if (st_sel == 0) g_out_speaker = !g_out_speaker;
      else g_mute[st_sel - 1] = !g_mute[st_sel - 1];
      board_audio_set_output(g_out_speaker);
      if (g_song_loaded) apply_mute(p);
      LCD_LOCK(); draw_settings_rows(); LCD_UNLOCK();
    }
  }
}

/* lowest-priority meter task: paints only on the Playback page */
void ui_lcd_task(void) {
  uint32_t last_frames = 0, last_drops = 0, last_cons = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(60));
    if (g_page != PG_PLAY || !g_song_loaded || !g_lcd_on) {
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
    last_drops = drops; last_cons = cons;
    unsigned pct = budget ? (unsigned)(sum * 100 / budget) : 0;
    LCD_LOCK();
    if (g_page == PG_PLAY && g_song_loaded && g_lcd_on) {
      draw_cpu_bar(snap, budget);
      draw_meter_numbers(pct, drpct);
      draw_dbg();
      board_lcd_present();
    }
    LCD_UNLOCK();
  }
}

void ui_run(void) {
  pfm_player *p = pfm_player_instance();
  g_mount_rc = f_mount(&g_fatfs, "", 1);
  g_have_sd = (g_mount_rc == FR_OK);
  strcpy(g_path, "/");
  board_audio_set_output(g_out_speaker);
  browse_reload();
  g_page = PG_BROWSER;
  LCD_LOCK(); draw_browser_full(); LCD_UNLOCK();

  int prevb = board_input_poll();
  TickType_t c_press = 0, last_nav = 0, last_yield = 0;
  int c_armed = !(prevb & BTN_CENTER), c_done = 0;
  for (;;) {
    int rendered = 0;
    if (g_song_loaded && board_audio_ring_fill() < (int)AUD_HIGH_WATER) {
      pfm_player_render(p, g_chunk, 512);
      board_audio_write(g_chunk, 512);
      g_render_frames += 512;
      rendered = 1;
    }
    if (g_muted_swap && board_audio_consumed_frames() >= g_unmute_cons) {
      board_audio_mute(0); g_muted_swap = 0;
    }
    int b = board_input_poll();
    int edge = b & ~prevb;
    TickType_t now = xTaskGetTickCount();
    if (b & BTN_CENTER) {
      if (c_armed && !c_done) {
        if (!c_press) c_press = now ? now : 1;
        else if ((now - c_press) >= pdMS_TO_TICKS(500)) { cycle_page(); c_done = 1; }
      }
    } else {
      if (c_armed && (prevb & BTN_CENTER) && !c_done && c_press) page_center(p);
      c_armed = 1; c_press = 0; c_done = 0;
    }
    if ((edge & (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT)) &&
        (now - last_nav) >= pdMS_TO_TICKS(110)) {
      page_nav(p, edge);
      last_nav = now;
    }
    prevb = b;
    /* Yield when idle, and force a slot at least every ~33 ms even when rendering
       flat-out, so the lowest-priority meter task can't be starved (a 1-tick nap
       drains ~55 frames; the ring has far more, so audio is unaffected). */
    if (!rendered || (now - last_yield) >= pdMS_TO_TICKS(33)) {
      vTaskDelay(1);
      last_yield = now;
    }
  }
}

#else  /* ---------------- simulator: flat flash browser + preview ------------- */

static void sim_play(int sel) {
  int len = board_storage_load(sel, g_songbuf, sizeof(g_songbuf));
  if (len <= 0) return;
  pfm_player *p = pfm_player_instance();
  pfm_player_init(p);
  if (!pfm_player_load(p, g_songbuf, (size_t)len)) return;
  board_audio_open(PFM_MIX_RATE, 0);
  uint32_t cap = PFM_MIX_RATE * PLAY_SECONDS, done = 0;
  while (done < cap) {
    pfm_player_render(p, g_chunk, 512);
    board_audio_write(g_chunk, 512);
    done += 512;
    if (board_input_poll() & BTN_CENTER) break;
  }
  board_audio_close();
}
void ui_run(void) {
  int n = board_storage_count(), sel = 0, top = 0;
  draw_list(sel, top, n); board_lcd_present();
  for (;;) {
    int ev = board_input_wait();
    if (!ev) break;
    if (ev == BTN_DOWN && sel < n - 1) sel++;
    else if (ev == BTN_UP && sel > 0) sel--;
    else if (ev == BTN_CENTER) sim_play(sel);
    if (sel < top) top = sel;
    if (sel >= top + VISROWS) top = sel - VISROWS + 1;
    draw_list(sel, top, n); board_lcd_present();
  }
}
#endif
