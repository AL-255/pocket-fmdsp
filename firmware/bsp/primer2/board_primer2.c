/*
 * Real STM32 Primer2 board backend (deployment skeleton).
 *
 * This implements the same board HAL as bsp/sim/board_sim.c, but against the
 * Primer2's actual peripherals (pins in primer2_pins.h, from the schematic).
 * It is NOT built for the QEMU sim (QEMU models none of these peripherals) — it
 * is the starting point for on-hardware bring-up. Bodies that need sizeable
 * drivers (SDIO+FatFs, I2S+DMA) are marked TODO; the LCD/GPIO paths show the
 * intended register-level approach.
 *
 * Clock: 12 MHz HSE -> PLL x6 -> 72 MHz SYSCLK.
 */
#include "board.h"
#include "primer2_pins.h"
#include <stdint.h>

/* --- Minimal register access (avoid pulling a full CMSIS device header). --- */
#define REG(a) (*(volatile uint32_t *)(a))
#define RCC_APB2ENR REG(0x40021018u)
#define RCC_AHBENR  REG(0x40021014u)

/* ---------------- LCD: FSMC 8080, ST7735-class 128x160 ---------------- */
/* Write a command/data pair to the panel over FSMC bank1 NE1 (RS=A16). */
static inline void lcd_cmd(uint16_t c) { PRIMER2_LCD_REG = c; }
static inline void lcd_dat(uint16_t d) { PRIMER2_LCD_DAT = d; }

static void lcd_set_window(int x, int y, int w, int h) {
  lcd_cmd(0x2A); lcd_dat(0); lcd_dat(x); lcd_dat(0); lcd_dat(x + w - 1); /* CASET */
  lcd_cmd(0x2B); lcd_dat(0); lcd_dat(y); lcd_dat(0); lcd_dat(y + h - 1); /* RASET */
  lcd_cmd(0x2C);                                                         /* RAMWR */
}

void board_lcd_clear(uint16_t color) {
  lcd_set_window(0, 0, BOARD_LCD_W, BOARD_LCD_H);
  for (int i = 0; i < BOARD_LCD_W * BOARD_LCD_H; i++) lcd_dat(color);
}
void board_lcd_pixel(int x, int y, uint16_t color) {
  if ((unsigned)x >= BOARD_LCD_W || (unsigned)y >= BOARD_LCD_H) return;
  lcd_set_window(x, y, 1, 1);
  lcd_dat(color);
}
void board_lcd_fill_rect(int x, int y, int w, int h, uint16_t color) {
  if (x < 0 || y < 0 || x + w > BOARD_LCD_W || y + h > BOARD_LCD_H) return;
  lcd_set_window(x, y, w, h);
  for (int i = 0; i < w * h; i++) lcd_dat(color);
}
void board_lcd_present(void) { /* GRAM is the display; nothing to flush */ }

/* ---------------- Joystick: GPIO read + simple edge detect ---------------- */
/* TODO: configure GPIOA/GPIOE as inputs; here we only sketch the read. */
static int joy_raw(void) {
  int m = 0;
  /* pseudo: read PE3/PE2/PE4/PE5 and PA8 (active per schematic pull config) */
  /* m |= gpio_get(GPIOE, PRIMER2_JOY_UP_PIN) ? BTN_UP : 0; ... */
  return m;
}
int board_input_wait(void) {
  static int prev;
  for (;;) {
    int cur = joy_raw();
    int edge = cur & ~prev;
    prev = cur;
    if (edge) return edge;
    /* TODO: WFI / debounce delay */
  }
}

/* ---------------- Audio: STW5094A codec via I2S2 + DMA ---------------- */
/* TODO: configure SPI2/I2S2 (PB12/13/15) + DMA double buffer; feed from
   pfm_player_render() in the DMA half/complete IRQ. */
void board_audio_open(unsigned rate, uint32_t total_frames) { (void)rate; (void)total_frames; }
void board_audio_write(const int16_t *stereo, size_t frames) { (void)stereo; (void)frames; }
void board_audio_close(void) {}

/* ---------------- Storage: microSD via SDIO 4-bit + FatFs ---------------- */
/* TODO: SDIO (PC8-12, PD2) + FatFs; enumerate *.M into the track table. */
int board_storage_count(void) { return 0; }
const char *board_storage_name(int i) { (void)i; return ""; }
const char *board_storage_group(int i) { (void)i; return ""; }
int board_storage_load(int i, uint8_t *buf, size_t maxlen) { (void)i; (void)buf; (void)maxlen; return -1; }

/* ---------------- lifecycle ---------------- */
void board_log(const char *s) { (void)s; /* TODO: route to USART or ITM */ }

void board_init(void) {
  /* TODO: clock tree (HSE 12MHz -> 72MHz), GPIO/FSMC/SDIO/I2S clocks, LCD init
     sequence (ST7735-class: SWRESET, SLPOUT, COLMOD=RGB565, MADCTL, DISPON). */
  (void)RCC_APB2ENR;
  (void)RCC_AHBENR;
}
