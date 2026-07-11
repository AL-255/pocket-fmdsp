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
/* bottom of every page: a control-hint bar, then a tab bar (File/Play/Config) */
#define TAB_H 9
#define TABBAR_Y (H - TAB_H)       /* current-tab bar sits at the very bottom */
#define HINT_Y (H - 2 * TAB_H)     /* control-hint bar sits just above it */

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
static int g_backlight = 5;          /* 0..BOARD_BL_MAX backlight brightness */
static int g_paused;                 /* 1 = playback paused (short tap C on play page) */

#ifdef PFM_RTOS
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)  /* Cortex-M DWT cycle counter */
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
  RGB_C(255, 70, 70),   /* FM     */
  RGB_C(90, 220, 90),   /* SSG    */
  RGB_C(80, 130, 255),  /* DRUM   */
  RGB_C(240, 210, 60),  /* PCM    */
  RGB_C(230, 90, 220),  /* SEQ    */
  RGB_C(60, 220, 220),  /* OUTPUT */
  RGB_C(255, 160, 60),  /* SD     */
};
static const char *const task_lbl[PFM_PROF_N] = {
  "FM", "SSG", "DRM", "PCM", "SEQ", "OUT", "SD",
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
  int y = HINT_Y - 11;
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

/* Redraw requests. The app task (prio 2, which also renders audio) NEVER draws:
   it mutates UI state and OR-s a bit here; the low-priority lcd task (prio 1)
   does the FSMC drawing. Because the render task out-prioritises the lcd task,
   drawing can no longer stall the render -> no ring underrun / sample-repeat
   glitch when browsing or switching pages. */
#define RDR_FULL 1u              /* full current-page redraw */
#define RDR_ROWS 2u              /* rows-only (browser/settings list) */
#define RDR_VOL  4u              /* volume bar (play page) */
static volatile uint32_t g_redraw;
static void ui_request(uint32_t bits) {
  taskENTER_CRITICAL();
  g_redraw |= bits;
  taskEXIT_CRITICAL();
}

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

/* playback mode: what to do when the current track finishes a play-through */
enum { PM_NORMAL = 0, PM_SINGLE = 1, PM_FLOOP = 2, PM_FRAND = 3, PM_N = 4 };
static const char *const pm_name[PM_N] = { "Normal", "Single Track Loop", "Folder Loop", "Folder Random" };
static int g_play_mode = PM_NORMAL;
static unsigned g_prev_lc;       /* previous loop count (for track-end edge detect) */
static uint32_t g_rng = 0x2545f491u; /* folder-random LCG state */

/* --- settings: hierarchical per-channel mute tree ---
   Output, then FM (folds open to FM0..FM5), SSG (folds open to SSG0..SSG2),
   Drum, PCM. Default enables FM0-2 + SSG0 (a PC-9801-26K-ish voicing at ~3-FM
   cost); the rest are muted and bypassed until the user turns them on. */
static uint8_t g_out_speaker;                          /* 0 headphone, 1 loudspeaker */
static uint8_t g_fm_mute[6]  = { 0, 0, 0, 1, 1, 1 };   /* FM0-2 on, FM3-5 muted */
static uint8_t g_ssg_mute[3] = { 0, 1, 1 };            /* SSG0 on, SSG1-2 muted */
static uint8_t g_drum_mute = 0;
static uint8_t g_pcm_mute  = 1;                        /* ppz8 PCM muted by default */
static int g_fm_open, g_ssg_open;                      /* folder expansion */
static int st_sel, st_top;                             /* selection index + scroll top */
static int g_cfg_status;                               /* Save row feedback: 0 none, 1 ok, -1 err */

enum { K_OUTPUT, K_BL, K_FM, K_FMCH, K_SSG, K_SSGCH, K_DRUM, K_PCM, K_SAVE, K_DEFAULT };
static struct { uint8_t kind, ch; } g_srow[20];
static int g_nsrow;
static void build_srows(void) {
  int n = 0;
  g_srow[n].kind = K_OUTPUT; g_srow[n++].ch = 0;
  g_srow[n].kind = K_BL;     g_srow[n++].ch = 0;
  g_srow[n].kind = K_FM;     g_srow[n++].ch = 0;
  if (g_fm_open) for (int c = 0; c < 6; c++) { g_srow[n].kind = K_FMCH; g_srow[n++].ch = c; }
  g_srow[n].kind = K_SSG;    g_srow[n++].ch = 0;
  if (g_ssg_open) for (int c = 0; c < 3; c++) { g_srow[n].kind = K_SSGCH; g_srow[n++].ch = c; }
  g_srow[n].kind = K_DRUM;   g_srow[n++].ch = 0;
  g_srow[n].kind = K_PCM;    g_srow[n++].ch = 0;
  g_srow[n].kind = K_SAVE;   g_srow[n++].ch = 0;
  g_srow[n].kind = K_DEFAULT;g_srow[n++].ch = 0;
  g_nsrow = n;
}
static int fm_on_count(void) { int n = 0; for (int c = 0; c < 6; c++) if (!g_fm_mute[c]) n++; return n; }
static int ssg_on_count(void) { int n = 0; for (int c = 0; c < 3; c++) if (!g_ssg_mute[c]) n++; return n; }

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

/* Dedicated highest-priority SD-read task. The app task posts a request (idx) and
   blocks; this task does the actual (blocking) FatFs read and posts back, so the
   read runs uninterrupted at the top priority. */
static TaskHandle_t g_sd_task, g_app_task;
static volatile int g_sd_req_idx, g_sd_result, g_sd_op;
enum { SDOP_LOAD = 0, SDOP_SAVECFG = 1 };
static int cfg_write_file(void);   /* forward decl; runs on the sd task */
void ui_set_task_handles(void *sd, void *app) {
  g_sd_task = (TaskHandle_t)sd; g_app_task = (TaskHandle_t)app;
}
void ui_sd_task(void) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   /* wait for a request */
    uint32_t t0 = DWT_CYCCNT;
    /* All FatFs access is serialised on this task (FatFs is single-threaded): song
       reads and the config write both run here so they can't collide. */
    if (g_sd_op == SDOP_SAVECFG) g_sd_result = cfg_write_file();
    else g_sd_result = src_load_idx(g_sd_req_idx);
    pfm_prof_cyc[PFM_PROF_SD] += DWT_CYCCNT - t0; /* show SD time in the CPU bar */
    if (g_app_task) xTaskNotifyGive(g_app_task);
  }
}
/* Called from the app task: run the SD read on the top-priority sd task and wait. */
static int sd_load(int idx) {
  if (!g_sd_task || !g_app_task) return src_load_idx(idx);  /* fallback */
  g_sd_op = SDOP_LOAD; g_sd_req_idx = idx;
  xTaskNotifyGive(g_sd_task);                  /* sd task preempts + reads */
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);     /* block until it's done */
  return g_sd_result;
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

/* ---------- config file (/fmdsp.cfg, ASCII key=value) ---------- */
static void cfg_defaults(void) {
  g_out_speaker = 0; g_volume = 5; g_backlight = 5;
  g_fm_mute[0]=0; g_fm_mute[1]=0; g_fm_mute[2]=0; g_fm_mute[3]=1; g_fm_mute[4]=1; g_fm_mute[5]=1;
  g_ssg_mute[0]=0; g_ssg_mute[1]=1; g_ssg_mute[2]=1;
  g_drum_mute = 0; g_pcm_mute = 1; g_play_mode = PM_NORMAL;
}
static char *cfg_putkv(char *d, const char *k, int v) {
  while (*k) *d++ = *k++;
  *d++ = '=';
  char t[6]; int n = 0;
  if (v <= 0) t[n++] = '0'; else { int x = v; while (x) { t[n++] = (char)('0' + x % 10); x /= 10; } }
  while (n) *d++ = t[--n];
  *d++ = '\n';
  return d;
}
static char *cfg_putbits(char *d, const char *k, const uint8_t *m, int n) {
  while (*k) *d++ = *k++;
  *d++ = '=';
  for (int c = 0; c < n; c++) *d++ = (char)('0' + (m[c] ? 1 : 0));
  *d++ = '\n';
  return d;
}
static int cfg_build(char *buf) {
  char *d = buf;
  d = cfg_putkv(d, "out", g_out_speaker);
  d = cfg_putkv(d, "vol", g_volume);
  d = cfg_putkv(d, "bl", g_backlight);
  d = cfg_putbits(d, "fm", g_fm_mute, 6);
  d = cfg_putbits(d, "ssg", g_ssg_mute, 3);
  d = cfg_putkv(d, "drum", g_drum_mute);
  d = cfg_putkv(d, "pcm", g_pcm_mute);
  d = cfg_putkv(d, "mode", g_play_mode);
  return (int)(d - buf);
}
static int cfg_geti(const char *v) {
  int n = 0; while (*v >= '0' && *v <= '9') { n = n * 10 + (*v - '0'); v++; } return n;
}
static int cfg_clamp(int x, int hi) { return x < 0 ? 0 : (x > hi ? hi : x); }
static void cfg_parse(char *buf, int len) {
  buf[len] = 0;
  char *p = buf;
  while (*p) {
    char *line = p;
    while (*p && *p != '\n' && *p != '\r') p++;
    char *eol = p;
    while (*p == '\n' || *p == '\r') p++;
    *eol = 0;
    char *eq = line; while (*eq && *eq != '=') eq++;
    if (*eq != '=') continue;
    *eq = 0; char *key = line, *val = eq + 1;
    if      (!strcmp(key, "out"))  g_out_speaker = cfg_geti(val) ? 1 : 0;
    else if (!strcmp(key, "vol"))  g_volume    = cfg_clamp(cfg_geti(val), BOARD_VOL_MAX);
    else if (!strcmp(key, "bl"))   g_backlight = cfg_clamp(cfg_geti(val), BOARD_BL_MAX);
    else if (!strcmp(key, "fm"))   for (int c = 0; c < 6 && val[c]; c++) g_fm_mute[c]  = (val[c] == '1');
    else if (!strcmp(key, "ssg"))  for (int c = 0; c < 3 && val[c]; c++) g_ssg_mute[c] = (val[c] == '1');
    else if (!strcmp(key, "drum")) g_drum_mute = cfg_geti(val) ? 1 : 0;
    else if (!strcmp(key, "pcm"))  g_pcm_mute  = cfg_geti(val) ? 1 : 0;
    else if (!strcmp(key, "mode")) { int x = cfg_geti(val); g_play_mode = (x >= 0 && x < PM_N) ? x : PM_NORMAL; }
  }
}
static int cfg_write_file(void) {   /* runs on the sd task (see ui_sd_task) */
  FIL fp;
  if (f_open(&fp, "/fmdsp.cfg", FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return 0;
  char buf[128];
  int n = cfg_build(buf);
  UINT bw = 0;
  int ok = (f_write(&fp, buf, (UINT)n, &bw) == FR_OK && bw == (UINT)n);
  f_close(&fp);
  return ok;
}
static void cfg_load(void) {   /* app task, at boot: no file -> keep defaults */
  FIL fp;
  if (f_open(&fp, "/fmdsp.cfg", FA_READ) != FR_OK) return;
  char buf[200];
  UINT br = 0;
  if (f_read(&fp, buf, sizeof(buf) - 1, &br) == FR_OK) cfg_parse(buf, (int)br);
  f_close(&fp);
}
static int cfg_save(void) {   /* app task: run the write on the sd task, mute across it */
  if (!g_sd_task || !g_app_task) return cfg_write_file();  /* sim fallback */
  int playing = g_song_loaded && !g_paused && !g_muted_swap;
  if (playing) board_audio_mute(1);
  g_sd_op = SDOP_SAVECFG;
  xTaskNotifyGive(g_sd_task);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  if (playing) board_audio_mute(0);
  return g_sd_result;
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
#define PLAY_DBG_Y (PLAY_LEG_Y + 38)   /* below the 3-row (7-entry) legend */
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
/* Bottom tab bar: File / Play / Config, current page highlighted. */
static void draw_tab_bar(void) {
  static const char *const tabn[PG_N] = { "File", "Play", "Config" };
  int tw = W / PG_N;
  for (int t = 0; t < PG_N; t++) {
    int x = t * tw, w = (t == PG_N - 1) ? (W - x) : tw;
    int on = (t == g_page);
    uint16_t bg = on ? COL_SEL_BG : COL_SUB_BG;
    uint16_t fg = on ? COL_SEL_FG : COL_NUM;
    board_lcd_fill_rect(x, TABBAR_Y, w, TAB_H, bg);
    int tx = x + (tw - (int)strlen(tabn[t]) * 4) / 2;   /* centre (4px glyphs) */
    gfx_text(tx, TABBAR_Y + 1, tabn[t], fg, bg);
  }
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
  { char ms[28]; strcpy(ms, g_paused ? "|| " : ""); strcat(ms, pm_name[g_play_mode]);
    gfx_text(2, PLAY_DBG_Y + 22, ms, g_paused ? COL_ERR : COL_ACCENT, COL_BG); }
  gfx_text(2, HINT_Y + 1, "tapC pause  2xC mode", COL_NUM, COL_BG);
  draw_tab_bar();
  board_lcd_present();
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
  board_lcd_fill_rect(0, HINT_Y, W, TAB_H, COL_TITLE_BG);
  gfx_text(2, HINT_Y + 1, "UD sel  LR dir  C play", COL_TITLE_FG, COL_TITLE_BG);
  draw_tab_bar();
  if (g_have_sd) draw_browser_rows();
  else board_lcd_present();
}
static void browse_reload(void) { b_count = src_count(); b_sel = 0; b_top = 0; }

/* ---------- settings screen ---------- */
static void draw_srow(int r) {   /* r = visible row 0..VISROWS-1 */
  int idx = st_top + r, y = LIST_Y + r * ROW_H;
  int sel = (idx == st_sel);
  uint16_t bg = sel ? COL_SEL_BG : COL_BG;
  board_lcd_fill_rect(0, y, W, ROW_H, bg);
  if (idx >= g_nsrow) return;
  uint16_t fg = sel ? COL_SEL_FG : COL_FG;
  int kind = g_srow[idx].kind, ch = g_srow[idx].ch;
  char lbl[10], val[10];
  int indent = 4, folder = 0, muted = 0;
  switch (kind) {
  case K_OUTPUT: strcpy(lbl, "Output"); strcpy(val, g_out_speaker ? "Speaker" : "Phones"); break;
  case K_FM:  folder = 1; strcpy(lbl, g_fm_open ? "-FM" : "+FM");
              val[0] = '0' + fm_on_count(); val[1] = '/'; val[2] = '6'; val[3] = 0; break;
  case K_SSG: folder = 1; strcpy(lbl, g_ssg_open ? "-SSG" : "+SSG");
              val[0] = '0' + ssg_on_count(); val[1] = '/'; val[2] = '3'; val[3] = 0; break;
  case K_FMCH:  indent = 14; lbl[0] = 'F'; lbl[1] = 'M'; lbl[2] = '0' + ch; lbl[3] = 0;
                muted = g_fm_mute[ch]; strcpy(val, muted ? "Mute" : "On"); break;
  case K_SSGCH: indent = 14; strcpy(lbl, "SSG"); lbl[3] = '0' + ch; lbl[4] = 0;
                muted = g_ssg_mute[ch]; strcpy(val, muted ? "Mute" : "On"); break;
  case K_DRUM: strcpy(lbl, "Drum"); muted = g_drum_mute; strcpy(val, muted ? "Mute" : "On"); break;
  case K_PCM:  strcpy(lbl, "PCM"); muted = g_pcm_mute; strcpy(val, muted ? "Mute" : "On"); break;
  case K_BL:   strcpy(lbl, "Backlight"); val[0] = 0; break;   /* value shown as a bar */
  case K_SAVE: strcpy(lbl, "Save"); strcpy(val, g_cfg_status == 1 ? "saved" : (g_cfg_status == -1 ? "err" : "to SD")); break;
  case K_DEFAULT: strcpy(lbl, "Default"); strcpy(val, "reset"); break;
  }
  gfx_text(indent, y + 1, lbl, fg, bg);
  if (kind == K_BL) {                          /* brightness slider bar */
    int bw = 44, fw = g_backlight * bw / BOARD_BL_MAX;
    board_lcd_fill_rect(70, y + 3, bw, 4, COL_SUB_BG);
    board_lcd_fill_rect(70, y + 3, fw, 4, sel ? COL_SEL_FG : COL_BAR);
    return;
  }
  uint16_t vc = sel ? COL_SEL_FG : (folder ? COL_SUB_FG : (muted ? COL_ERR : COL_ACCENT));
  gfx_text(70, y + 1, val, vc, bg);
}
static void draw_settings_rows(void) {
  build_srows();
  if (st_sel >= g_nsrow) st_sel = g_nsrow - 1;
  if (st_sel < 0) st_sel = 0;
  if (st_sel < st_top) st_top = st_sel;
  if (st_sel >= st_top + VISROWS) st_top = st_sel - VISROWS + 1;
  for (int r = 0; r < VISROWS; r++) draw_srow(r);
  board_lcd_present();
}
static void draw_settings_full(void) {
  board_lcd_clear(COL_BG);
  board_lcd_fill_rect(0, 0, W, TITLE_H, COL_TITLE_BG);
  gfx_text(2, 4, "Settings", COL_TITLE_FG, COL_TITLE_BG);
  board_lcd_fill_rect(0, HINT_Y, W, TAB_H, COL_TITLE_BG);
  gfx_text(2, HINT_Y + 1, "UD move  LR open  C set", COL_TITLE_FG, COL_TITLE_BG);
  draw_tab_bar();
  draw_settings_rows();
}

/* ---------- redraw dispatch (lcd task only) ---------- */
static void draw_current_full(void) {
  if (g_page == PG_BROWSER) draw_browser_full();
  else if (g_page == PG_PLAY) draw_play_chrome();
  else draw_settings_full();
}
static void draw_current_rows(void) {
  if (g_page == PG_BROWSER) draw_browser_rows();
  else if (g_page == PG_SETTINGS) draw_settings_rows();
}

/* ---------- audio control ---------- */
static void apply_mute(pfm_player *p) {
  unsigned m = 0;                                 /* OPNA mask: FM 0-5, SSG 6-8, drum 9-14 */
  for (int c = 0; c < 6; c++) if (g_fm_mute[c])  m |= (1u << c);
  for (int c = 0; c < 3; c++) if (g_ssg_mute[c]) m |= (1u << (6 + c));
  if (g_drum_mute) m |= 0x7e00u;                  /* all 6 rhythm voices */
  pfm_player_set_mask(p, m, g_pcm_mute);
}
/* Toggle a leaf setting (output / a channel / drum / PCM) and re-apply. */
static void settings_toggle_leaf(pfm_player *p, int kind, int ch) {
  switch (kind) {
  case K_OUTPUT: g_out_speaker = !g_out_speaker; board_audio_set_output(g_out_speaker); break;
  case K_FMCH:   g_fm_mute[ch]  = !g_fm_mute[ch];  break;
  case K_SSGCH:  g_ssg_mute[ch] = !g_ssg_mute[ch]; break;
  case K_DRUM:   g_drum_mute = !g_drum_mute; break;
  case K_PCM:    g_pcm_mute  = !g_pcm_mute;  break;
  default: return;
  }
  if (g_song_loaded) apply_mute(p);
  ui_request(RDR_ROWS);
}
/* Center on the selected settings row: fold FM/SSG open|closed, else toggle. */
static void settings_activate(pfm_player *p) {
  build_srows();
  if (st_sel >= g_nsrow) st_sel = g_nsrow - 1;
  int kind = g_srow[st_sel].kind, ch = g_srow[st_sel].ch;
  if (kind == K_FM)  { g_fm_open  = !g_fm_open;  ui_request(RDR_FULL); }
  else if (kind == K_SSG) { g_ssg_open = !g_ssg_open; ui_request(RDR_FULL); }
  else if (kind == K_BL) { g_backlight = (g_backlight + 1) % (BOARD_BL_MAX + 1);
                           board_lcd_backlight(g_backlight); ui_request(RDR_ROWS); }
  else if (kind == K_SAVE) { g_cfg_status = cfg_save() ? 1 : -1; ui_request(RDR_ROWS); }
  else if (kind == K_DEFAULT) {                 /* restore factory config in memory only */
    cfg_defaults();
    board_audio_set_output(g_out_speaker);
    board_audio_set_volume(g_volume);
    board_lcd_backlight(g_backlight);
    if (g_song_loaded) apply_mute(p);
    g_cfg_status = 0; ui_request(RDR_FULL);
  }
  else settings_toggle_leaf(p, kind, ch);
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
  int len = sd_load(idx);                      /* SD read on the top-priority sd task */
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
  g_paused = 0;                 /* a freshly-started track always plays */
  g_prev_lc = 0;               /* re-arm end-of-track detection for the new track */
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
  if (g_page == PG_PLAY) ui_request(RDR_FULL);
}
/* Folder Random: jump to a random (different) track in the playing folder. */
static void play_random(pfm_player *p) {
  if (!g_song_loaded) return;
  g_rng = g_rng * 1664525u + 1013904223u + DWT_CYCCNT;
  char saved[256]; strcpy(saved, g_path); strcpy(g_path, g_play_path);
  int cnt = src_count(), found = -1, start = cnt ? (int)(g_rng % (unsigned)cnt) : 0;
  for (int i = 0; i < cnt; i++) {
    int idx = (start + i) % cnt;
    char nm[BR_NAMELEN]; int isd;
    if (src_entry(idx, nm, &isd) == 0 && !isd && idx != g_play_idx) { found = idx; break; }
  }
  if (found >= 0) start_song(p, found);   /* else: only one track -> keep looping */
  strcpy(g_path, saved);
  if (g_page == PG_PLAY) ui_request(RDR_FULL);
}
/* Normal: advance to the next track, stopping (last track keeps looping) at the end. */
static void play_next_stop(pfm_player *p) {
  if (!g_song_loaded) return;
  char saved[256]; strcpy(saved, g_path); strcpy(g_path, g_play_path);
  int cnt = src_count(), found = -1;
  for (int i = g_play_idx + 1; i < cnt; i++) {
    char nm[BR_NAMELEN]; int isd;
    if (src_entry(i, nm, &isd) == 0 && !isd) { found = i; break; }
  }
  if (found >= 0) start_song(p, found);
  strcpy(g_path, saved);
  if (g_page == PG_PLAY) ui_request(RDR_FULL);
}
/* Called once when the current track finishes a play-through (see the render loop). */
static void play_advance(pfm_player *p) {
  switch (g_play_mode) {
  case PM_SINGLE: break;                     /* let the track loop naturally */
  case PM_FLOOP:  play_step(p, +1); break;   /* next, wrapping to the first */
  case PM_FRAND:  play_random(p); break;
  case PM_NORMAL: default: play_next_stop(p); break;
  }
}
static void cycle_play_mode(void) {
  g_play_mode = (g_play_mode + 1) % PM_N;
  g_prev_lc = 0;                              /* re-arm end-of-track for the new mode */
  ui_request(RDR_FULL);
}
static void toggle_pause(void) {
  g_paused = !g_paused;
  if (g_paused) memset(g_chunk, 0, sizeof g_chunk); /* the paused feed pushes silence */
  board_audio_mute(g_paused ? 1 : 0);
  ui_request(RDR_FULL);
}

/* ---------- page dispatch ---------- */
static void cycle_page(void) {
  g_page = (g_page + 1) % PG_N;
  ui_request(RDR_FULL);
}
static void page_center(pfm_player *p) {
  if (g_page == PG_BROWSER) {
    char name[BR_NAMELEN]; int isd;
    if (src_entry(b_sel, name, &isd) != 0) return;
    if (isd) { path_push(name); browse_reload(); ui_request(RDR_FULL); }
    else { start_song(p, b_sel); g_page = PG_PLAY; ui_request(RDR_FULL); }
  } else if (g_page == PG_SETTINGS) {   /* open/close folder or change value */
    settings_activate(p);
  }
  /* PG_PLAY short/double taps (pause / mode) are handled in the input loop. */
}
static void page_nav(pfm_player *p, int edge) {
  if (g_page == PG_BROWSER) {
    int full = 0;
    if (edge & BTN_DOWN) { if (b_count > 0) b_sel = (b_sel + 1) % b_count; }        /* wrap */
    else if (edge & BTN_UP) { if (b_count > 0) b_sel = (b_sel + b_count - 1) % b_count; } /* wrap */
    else if (edge & BTN_RIGHT) {       /* descend */
      char name[BR_NAMELEN]; int isd;
      if (g_have_sd && src_entry(b_sel, name, &isd) == 0 && isd) { path_push(name); browse_reload(); full = 1; }
    } else if (edge & BTN_LEFT) {      /* ascend */
      if (g_have_sd) { path_pop(); browse_reload(); full = 1; }
    }
    if (b_sel < b_top) b_top = b_sel;
    if (b_sel >= b_top + VISROWS) b_top = b_sel - VISROWS + 1;
    ui_request(full ? RDR_FULL : RDR_ROWS);
  } else if (g_page == PG_PLAY) {
    if (edge & (BTN_UP | BTN_DOWN)) {
      if ((edge & BTN_UP) && g_volume < BOARD_VOL_MAX) g_volume++;
      else if ((edge & BTN_DOWN) && g_volume > 0) g_volume--;
      board_audio_set_volume(g_volume);
      ui_request(RDR_VOL);
    } else if (edge & BTN_RIGHT) play_step(p, +1);
    else if (edge & BTN_LEFT) play_step(p, -1);
  } else {                              /* settings — same scheme as the file browser:
                                           UD move (wrap), LR enter/exit submenus, C changes value */
    build_srows();
    if (st_sel >= g_nsrow) st_sel = g_nsrow - 1;
    if (edge & BTN_DOWN) { st_sel = (st_sel + 1) % g_nsrow; ui_request(RDR_ROWS); }        /* wrap */
    else if (edge & BTN_UP) { st_sel = (st_sel + g_nsrow - 1) % g_nsrow; ui_request(RDR_ROWS); } /* wrap */
    else if (edge & (BTN_LEFT | BTN_RIGHT)) {
      int kind = g_srow[st_sel].kind;
      if (kind == K_FM || kind == K_SSG) {              /* right opens, left closes the folder */
        int *open = (kind == K_FM) ? &g_fm_open : &g_ssg_open;
        int want = (edge & BTN_RIGHT) ? 1 : 0;
        if (*open != want) { *open = want; ui_request(RDR_FULL); }
      } else if (kind == K_BL) {                        /* brightness slider: L dimmer, R brighter */
        if ((edge & BTN_RIGHT) && g_backlight < BOARD_BL_MAX) g_backlight++;
        else if ((edge & BTN_LEFT) && g_backlight > 0) g_backlight--;
        board_lcd_backlight(g_backlight); ui_request(RDR_ROWS);
      } else if (edge & BTN_LEFT) {                     /* left inside a folder exits to it */
        if (kind == K_FMCH)  { g_fm_open = 0;  st_sel = 2; ui_request(RDR_FULL); }
        else if (kind == K_SSGCH) { g_ssg_open = 0; st_sel = 3 + (g_fm_open ? 6 : 0); ui_request(RDR_FULL); }
      }
      /* right on a channel, or L/R on Output/Drum/PCM/Save/Default: no-op — use C */
    }
  }
}

/* lowest-priority draw task: services redraw requests (browser/settings/pages)
   AND paints the meter on the Playback page. All FSMC drawing lives here, below
   the render task, so a draw can never starve the codec ring. */
void ui_lcd_task(void) {
  uint32_t last_frames = 0, last_drops = 0, last_cons = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(30));

    /* 1. explicit redraw requests from the app task */
    uint32_t req;
    taskENTER_CRITICAL(); req = g_redraw; g_redraw = 0; taskEXIT_CRITICAL();
    if (req) {
      LCD_LOCK();
      if (req & RDR_FULL) draw_current_full();
      else {
        if (req & RDR_ROWS) draw_current_rows();
        if ((req & RDR_VOL) && g_page == PG_PLAY) { draw_vol(g_volume); board_lcd_present(); }
      }
      LCD_UNLOCK();
    }

    /* 2. live meter (play page only) */
    /* Skip while swapping songs: the load stalls the producer (muted, so silent)
       and would otherwise register as CPU/DR noise. Re-baseline and wait. */
    if (g_page != PG_PLAY || !g_song_loaded || g_paused || g_muted_swap) {
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
    if (g_page == PG_PLAY && g_song_loaded && !g_paused) {
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
  cfg_defaults();                       /* factory config, then overlay the saved file */
  if (g_have_sd) cfg_load();            /* restore /fmdsp.cfg if present */
  board_lcd_backlight(g_backlight);
  board_audio_set_output(g_out_speaker);
  /* default browse dir: /fmdsp if it exists, else / */
  strcpy(g_path, "/");
  if (g_have_sd) {
    DIR d;
    if (f_opendir(&d, "/fmdsp") == FR_OK) { f_closedir(&d); strcpy(g_path, "/fmdsp"); }
  }
  browse_reload();
  g_page = PG_BROWSER;
  ui_request(RDR_FULL);

  int prevb = board_input_poll();
  TickType_t c_press = 0, last_nav = 0, last_yield = 0, held_since = 0, pending_tap = 0;
  int c_armed = !(prevb & BTN_CENTER), c_done = 0, held_dir = 0;
  for (;;) {
    int rendered = 0;
    if (g_song_loaded && board_audio_ring_fill() < (int)AUD_HIGH_WATER) {
      /* Keep feeding the ring even while paused (silence). If the producer stops,
         the DMA read pointer wraps unmetered and the consumed counter drifts, so
         ring_fill reads high forever and playback never resumes. */
      if (!g_paused) { pfm_player_render(p, g_chunk, 512); g_render_frames += 512; }
      board_audio_write(g_chunk, 512);
      rendered = 1;
    }
    if (g_muted_swap && board_audio_consumed_frames() >= g_unmute_cons) {
      board_audio_mute(0); g_muted_swap = 0;
    }
    /* End of track -> playback-mode action (once per play-through). Skipped while
       paused or mid-swap; play_advance() may start a new track (loop count reset). */
    if (g_song_loaded && !g_paused && !g_muted_swap) {
      unsigned lc = pfm_player_loopcount(p);
      if (lc >= 1 && g_prev_lc < 1) play_advance(p);
      g_prev_lc = pfm_player_loopcount(p);
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
      if (c_armed && (prevb & BTN_CENTER) && !c_done && c_press) {
        /* short tap. On the play page a single tap toggles pause and a double tap
           cycles the playback mode, so the single action is deferred ~300 ms to
           tell them apart; other pages act immediately. */
        if (g_page == PG_PLAY) {
          if (pending_tap && (now - pending_tap) < pdMS_TO_TICKS(300)) {
            cycle_play_mode(); pending_tap = 0;
          } else pending_tap = now ? now : 1;
        } else page_center(p);
      }
      c_armed = 1; c_press = 0; c_done = 0;
    }
    if (pending_tap && (now - pending_tap) >= pdMS_TO_TICKS(300)) {
      if (g_page == PG_PLAY) toggle_pause();     /* confirmed single tap: pause/resume */
      pending_tap = 0;
    }
    /* Nav: the initial press fires immediately (110 ms debounce); then while UP or
       DOWN is held past 0.5 s it auto-repeats every 70 ms (rapid fire). LR don't
       auto-repeat. */
    int nav = edge & (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT);
    int ud = b & (BTN_UP | BTN_DOWN);
    if (ud == BTN_UP || ud == BTN_DOWN) {
      if (held_dir != ud) { held_dir = ud; held_since = now; }
    } else held_dir = 0;
    if (nav) {
      if ((now - last_nav) >= pdMS_TO_TICKS(110)) { page_nav(p, nav); last_nav = now; }
    } else if (held_dir && (now - held_since) >= pdMS_TO_TICKS(500) &&
               (now - last_nav) >= pdMS_TO_TICKS(70)) {
      page_nav(p, held_dir);
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
