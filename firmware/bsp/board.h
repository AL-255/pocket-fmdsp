#ifndef PFM_BOARD_H
#define PFM_BOARD_H
/*
 * Board hardware-abstraction layer. The GUI + player run against this; two
 * backends implement it:
 *   - bsp/sim/board_sim.c      : QEMU/semihosting (LCD->PPM, audio->WAV,
 *                                joystick->scripted input, SD->host files)
 *   - bsp/primer2/board_primer2.c : real STM32 Primer2 peripherals (FSMC LCD,
 *                                GPIO joystick, I2S codec, SDIO microSD)
 */
#include <stdint.h>
#include <stddef.h>

#define BOARD_LCD_W 128
#define BOARD_LCD_H 160

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

/* Joystick edge events (bitmask). */
enum {
  BTN_UP = 1, BTN_DOWN = 2, BTN_LEFT = 4, BTN_RIGHT = 8, BTN_CENTER = 16,
};

/* ---- lifecycle ---- */
void board_init(void);
void board_log(const char *s);

/* ---- LCD (128x160, RGB565) ---- */
void board_lcd_clear(uint16_t color);
void board_lcd_fill_rect(int x, int y, int w, int h, uint16_t color);
void board_lcd_pixel(int x, int y, uint16_t color);
void board_lcd_present(void); /* sim: emit a screenshot; real: no-op (GRAM live) */

/* ---- input ---- */
/* Block until the next joystick event; returns a BTN_* value.
   In the sim this returns the next scripted event, or 0 when the script ends. */
int board_input_wait(void);
/* Non-blocking: bitmask of buttons currently held (no debounce/edge). Sim: 0. */
int board_input_poll(void);

/* ---- perf timing ---- */
/* Free-running CPU cycle counter (wraps at 2^32) and the core clock in Hz.
   Used to display a real-time render factor. Sim returns dummy values. */
uint32_t board_cycles(void);
uint32_t board_cpu_hz(void);

/* ---- audio out (interleaved stereo int16) ---- */
void board_audio_open(unsigned samplerate, uint32_t total_frames);
void board_audio_write(const int16_t *stereo, size_t frames);
void board_audio_close(void);

/* ---- storage (the song "SD card") ---- */
int board_storage_count(void);              /* number of tracks */
const char *board_storage_name(int index);  /* display name (basename, ASCII) */
const char *board_storage_group(int index); /* section label (Shift-JIS), may be "" */
/* Load track `index` into buf (<= maxlen). Returns byte length, or -1. */
int board_storage_load(int index, uint8_t *buf, size_t maxlen);

#endif /* PFM_BOARD_H */
