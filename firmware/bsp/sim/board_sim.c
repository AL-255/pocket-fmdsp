/*
 * QEMU/semihosting board backend. Implements the board HAL against the host so
 * the same GUI + player firmware that runs on the Primer2 can be exercised and
 * observed under QEMU:
 *   - LCD    -> RGB565 framebuffer, dumped as PPM screenshots on present()
 *   - input  -> a scripted event sequence read from a host file
 *   - audio  -> streamed to a host WAV file
 *   - storage-> the song "SD card" is a host directory listed by a manifest file
 *
 * Host-side control files (in QEMU's working directory):
 *   sim_tracklist.txt : one track per line, "<path>\t<group-SJIS>"
 *   sim_input.txt     : joystick script, chars U/D/L/R/C (others ignored)
 * Outputs: sim_shot_NNN.ppm, sim_play_NNN.wav
 */
#include "board.h"
#include "semihosting.h"

/* ---------- config / sizes (RAM-budgeted for 128 KB sim) ---------- */
#define MAX_TRACKS 120
#define PATH_MAX 64
#define GROUP_MAX 32

/* ---------- LCD framebuffer ---------- */
static uint16_t g_fb[BOARD_LCD_W * BOARD_LCD_H];
static int g_shot_no;

void board_lcd_clear(uint16_t color) {
  for (int i = 0; i < BOARD_LCD_W * BOARD_LCD_H; i++) g_fb[i] = color;
}
void board_lcd_pixel(int x, int y, uint16_t color) {
  if ((unsigned)x < BOARD_LCD_W && (unsigned)y < BOARD_LCD_H)
    g_fb[y * BOARD_LCD_W + x] = color;
}
void board_lcd_fill_rect(int x, int y, int w, int h, uint16_t color) {
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++) board_lcd_pixel(x + i, y + j, color);
}

static void num3(char *d, int n) { /* NNN into d[0..2] */
  d[0] = '0' + (n / 100) % 10;
  d[1] = '0' + (n / 10) % 10;
  d[2] = '0' + n % 10;
}

void board_lcd_backlight(int level) { (void)level; } /* sim: no panel */
void board_lcd_present(void) {
  char path[] = "sim_shot_000.ppm";
  num3(path + 9, g_shot_no++);
  int fd = sh_open(path, 5 /* wb */);
  if (fd < 0) return;
  const char hdr[] = "P6\n128 160\n255\n";
  sh_write(fd, hdr, sizeof(hdr) - 1);
  uint8_t row[BOARD_LCD_W * 3];
  for (int y = 0; y < BOARD_LCD_H; y++) {
    for (int x = 0; x < BOARD_LCD_W; x++) {
      uint16_t p = g_fb[y * BOARD_LCD_W + x];
      row[x * 3 + 0] = (uint8_t)((p >> 11) << 3);        /* R5 -> R8 */
      row[x * 3 + 1] = (uint8_t)(((p >> 5) & 0x3f) << 2); /* G6 -> G8 */
      row[x * 3 + 2] = (uint8_t)((p & 0x1f) << 3);        /* B5 -> B8 */
    }
    sh_write(fd, row, sizeof(row));
  }
  sh_close(fd);
  board_log("[sim] screenshot ");
  board_log(path);
  board_log("\n");
}

/* ---------- host file helper ---------- */
static long read_host_file(const char *path, void *buf, long max) {
  int fd = sh_open(path, 1 /* rb */);
  if (fd < 0) return -1;
  long len = sh_flen(fd);
  if (len < 0 || len > max) len = max;
  int notread = sh_read(fd, buf, len);
  sh_close(fd);
  return len - (notread < 0 ? len : notread);
}

/* ---------- input script ---------- */
static char g_input[512];
static int g_input_len, g_input_pos;

static void input_init(void) {
  long n = read_host_file("sim_input.txt", g_input, sizeof(g_input));
  g_input_len = n > 0 ? (int)n : 0;
  g_input_pos = 0;
}

int board_input_wait(void) {
  while (g_input_pos < g_input_len) {
    char c = g_input[g_input_pos++];
    switch (c) {
    case 'U': case 'u': return BTN_UP;
    case 'D': case 'd': return BTN_DOWN;
    case 'L': case 'l': return BTN_LEFT;
    case 'R': case 'r': return BTN_RIGHT;
    case 'C': case 'c': return BTN_CENTER;
    default: break; /* whitespace/comment */
    }
  }
  return 0; /* script exhausted -> app exits */
}

/* Sim has no live input polling; report nothing held so playback runs to its
   frame limit, and a fixed nominal clock so the perf meter has sane numbers. */
int board_input_poll(void) { return 0; }
uint32_t board_cycles(void) { static uint32_t c; return (c += 1000000u); }
uint32_t board_cpu_hz(void) { return 96000000u; }

/* ---------- audio WAV ---------- */
static int g_wav_fd = -1, g_wav_no;

static void put32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void put16(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }

void board_audio_set_volume(int level) { (void)level; }
void board_audio_mute(int on) { (void)on; }
void board_audio_set_output(int speaker) { (void)speaker; }
void board_audio_restart(void) {}
uint32_t board_sd_read_errors(void) { return 0; }
uint32_t board_audio_underruns(void) { return 0; }
uint32_t board_audio_consumed_frames(void) { return 0; }
int32_t board_audio_ring_fill(void) { return 1 << 20; } /* sim: always draw */

void board_audio_open(unsigned rate, uint32_t total_frames) {
  char path[] = "sim_play_000.wav";
  num3(path + 9, g_wav_no++);
  g_wav_fd = sh_open(path, 5 /* wb */);
  if (g_wav_fd < 0) return;
  uint32_t data = total_frames * 4;
  uint8_t h[44];
  __builtin_memcpy(h, "RIFF", 4); put32(h+4, 36 + data); __builtin_memcpy(h+8, "WAVE", 4);
  __builtin_memcpy(h+12, "fmt ", 4); put32(h+16, 16); put16(h+20, 1); put16(h+22, 2);
  put32(h+24, rate); put32(h+28, rate*4); put16(h+32, 4); put16(h+34, 16);
  __builtin_memcpy(h+36, "data", 4); put32(h+40, data);
  sh_write(g_wav_fd, h, 44);
  board_log("[sim] audio -> ");
  board_log(path);
  board_log("\n");
}
void board_audio_write(const int16_t *stereo, size_t frames) {
  if (g_wav_fd >= 0) sh_write(g_wav_fd, stereo, frames * 4);
}
void board_audio_close(void) {
  if (g_wav_fd >= 0) sh_close(g_wav_fd);
  g_wav_fd = -1;
}

/* ---------- storage (manifest of host files) ---------- */
static char g_path[MAX_TRACKS][PATH_MAX];
static char g_group[MAX_TRACKS][GROUP_MAX];
static int g_ntracks;

static void basename_noext(const char *path, char *out, int outmax) {
  const char *base = path;
  for (const char *p = path; *p; p++)
    if (*p == '/' || *p == '\\') base = p + 1;
  int i = 0;
  while (base[i] && i < outmax - 1) { out[i] = base[i]; i++; }
  out[i] = 0;
  /* strip trailing ".M"/".m" */
  if (i >= 2 && out[i-2] == '.' && (out[i-1] == 'M' || out[i-1] == 'm')) out[i-2] = 0;
}

static void storage_init(void) {
  static char buf[10240];
  long n = read_host_file("sim_tracklist.txt", buf, sizeof(buf) - 1);
  g_ntracks = 0;
  if (n <= 0) return;
  buf[n] = 0;
  int i = 0;
  while (i < n && g_ntracks < MAX_TRACKS) {
    /* parse a line: path \t group */
    int ps = i;
    while (i < n && buf[i] != '\t' && buf[i] != '\n' && buf[i] != '\r') i++;
    int pe = i;
    int gs = -1, ge = -1;
    if (i < n && buf[i] == '\t') {
      i++;
      gs = i;
      while (i < n && buf[i] != '\n' && buf[i] != '\r') i++;
      ge = i;
    }
    while (i < n && (buf[i] == '\n' || buf[i] == '\r')) i++;
    if (pe > ps) {
      int t = g_ntracks++;
      int L = pe - ps < PATH_MAX - 1 ? pe - ps : PATH_MAX - 1;
      for (int k = 0; k < L; k++) g_path[t][k] = buf[ps + k];
      g_path[t][L] = 0;
      if (gs >= 0) {
        int G = ge - gs < GROUP_MAX - 1 ? ge - gs : GROUP_MAX - 1;
        for (int k = 0; k < G; k++) g_group[t][k] = buf[gs + k];
        g_group[t][G] = 0;
      } else {
        g_group[t][0] = 0;
      }
    }
  }
}

int board_storage_count(void) { return g_ntracks; }
const char *board_storage_name(int i) {
  static char scratch[PATH_MAX]; /* valid until the next call — UI uses it at once */
  if (i < 0 || i >= g_ntracks) return "";
  basename_noext(g_path[i], scratch, PATH_MAX);
  return scratch;
}
const char *board_storage_group(int i) {
  return (i >= 0 && i < g_ntracks) ? g_group[i] : "";
}
int board_storage_load(int i, uint8_t *buf, size_t maxlen) {
  if (i < 0 || i >= g_ntracks) return -1;
  return (int)read_host_file(g_path[i], buf, (long)maxlen);
}

/* ---------- lifecycle ---------- */
void board_log(const char *s) { sh_write0(s); }

void board_init(void) {
  input_init();
  storage_init();
  board_log("[sim] board init: ");
  board_log("tracks loaded\n");
}
