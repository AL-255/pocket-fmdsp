/*
 * Real STM32 Primer2 board backend.
 *
 * Register-level, no vendor SPL. The LCD path (FSMC bank NORSRAM3 @0x68000000,
 * GPIO chip-select, ST7732 controller, byte<<4 on the 16-bit bus) is taken from
 * the CircleOS 4.62 driver (Circle/OS/Hardware/Primer_1_2/lcd_spe.c), which is
 * the authoritative source for this panel.
 *
 * Clock: 12 MHz HSE -> PLL x6 -> 72 MHz.
 * Tracks live in flash (gen/songs.h); no SD driver yet. Audio is not wired up
 * yet (the codec needs I2C+I2S bring-up), so board_audio_* are no-ops: the
 * player still renders, which exercises the DSP and drives the VU bar.
 */
#include "board.h"
#include "primer2_pins.h"
#include "songs.h"
#include <stdint.h>

/* ---------------- registers ---------------- */
#define REG32(a) (*(volatile uint32_t *)(a))

#define RCC_CR       REG32(0x40021000)
#define RCC_CFGR     REG32(0x40021004)
#define RCC_APB2ENR  REG32(0x40021018)
#define RCC_AHBENR   REG32(0x40021014)
#define FLASH_ACR    REG32(0x40022000)

#define GPIOA_BASE 0x40010800u
#define GPIOB_BASE 0x40010C00u
#define GPIOD_BASE 0x40011400u
#define GPIOE_BASE 0x40011800u
#define GPIO_CRL(b) REG32((b) + 0x00)
#define GPIO_CRH(b) REG32((b) + 0x04)
#define GPIO_IDR(b) REG32((b) + 0x08)
#define GPIO_ODR(b) REG32((b) + 0x0C)
#define GPIO_BSRR(b) REG32((b) + 0x10)

#define FSMC_BCR3 REG32(0xA0000010)
#define FSMC_BTR3 REG32(0xA0000014)

#define SYST_CSR  REG32(0xE000E010)
#define SYST_RVR  REG32(0xE000E014)
#define SYST_CVR  REG32(0xE000E018)

/* GPIO 4-bit configs */
#define CNF_OUT_PP_50 0x3u  /* MODE=11 CNF=00 */
#define CNF_AF_PP_50  0xBu  /* MODE=11 CNF=10 */
#define CNF_IN_PULL   0x8u  /* MODE=00 CNF=10 (pull dir from ODR) */

static void pin_cfg(uint32_t base, int pin, uint32_t cfg) {
  volatile uint32_t *r = (pin < 8) ? &GPIO_CRL(base) : &GPIO_CRH(base);
  int s = (pin & 7) * 4;
  *r = (*r & ~(0xFu << s)) | (cfg << s);
}
static void pin_set(uint32_t base, int pin, int v) {
  GPIO_BSRR(base) = v ? (1u << pin) : (1u << (pin + 16));
}
static int pin_get(uint32_t base, int pin) { return (GPIO_IDR(base) >> pin) & 1u; }

/* ---------------- clock + delay ---------------- */
static void clock_init(void) {
  RCC_CR |= (1u << 16);                       /* HSEON */
  for (volatile int t = 0; t < 200000; t++)   /* wait HSERDY, bounded */
    if (RCC_CR & (1u << 17)) break;
  FLASH_ACR = 0x12;                            /* prefetch + 2 wait states */
  /* PLLSRC=HSE, PLLXTPRE=0, PLLMUL=x6 (0100), APB1=/2, APB2=/1, AHB=/1 */
  RCC_CFGR = (1u << 16) | (0x4u << 18) | (0x4u << 8);
  RCC_CR |= (1u << 24);                        /* PLLON */
  for (volatile int t = 0; t < 200000; t++)
    if (RCC_CR & (1u << 25)) break;            /* PLLRDY */
  RCC_CFGR |= 0x2u;                            /* SW = PLL */
  for (volatile int t = 0; t < 200000; t++)
    if ((RCC_CFGR & 0xCu) == 0x8u) break;      /* SWS = PLL */
}

static void delay_ms(uint32_t ms) {
  SYST_RVR = 72000u - 1u;
  SYST_CVR = 0;
  SYST_CSR = 5u; /* enable, core clock, no IRQ */
  while (ms--) {
    while (!(SYST_CSR & (1u << 16))) {} /* COUNTFLAG */
  }
  SYST_CSR = 0;
}

/* ---------------- LCD (ST7732 over FSMC) ---------------- */
#define LCD_CMD_ADDR PRIMER2_LCD_CMD_ADDR
#define LCD_DAT_ADDR PRIMER2_LCD_DAT_ADDR
/* DB0..DB7 are wired to FSMC_D4..D11 -> every byte goes out as (b << 4). */
static inline void lcd_cmd(uint8_t c) { *(volatile uint16_t *)LCD_CMD_ADDR = (uint16_t)(c << 4); }
static inline void lcd_dat(uint8_t d) { *(volatile uint16_t *)LCD_DAT_ADDR = (uint16_t)(d << 4); }

static void fsmc_init(void) {
  RCC_AHBENR |= (1u << 8); /* FSMCEN */
  /* data: PE7..PE14 AF-PP */
  for (int p = 7; p <= 14; p++) pin_cfg(GPIOE_BASE, p, CNF_AF_PP_50);
  /* control: PD4=NOE, PD5=NWE, PD11=A16 (RS) AF-PP */
  pin_cfg(GPIOD_BASE, 4, CNF_AF_PP_50);
  pin_cfg(GPIOD_BASE, 5, CNF_AF_PP_50);
  pin_cfg(GPIOD_BASE, 11, CNF_AF_PP_50);
  /* RST(PD6) and CS(PD7) are plain GPIO outputs; CS stays low (always selected) */
  pin_cfg(GPIOD_BASE, PRIMER2_LCD_RST_PIN, CNF_OUT_PP_50);
  pin_cfg(GPIOD_BASE, 7, CNF_OUT_PP_50);
  pin_set(GPIOD_BASE, PRIMER2_LCD_RST_PIN, 1);
  pin_set(GPIOD_BASE, 7, 0);

  /* ADDSET=2 ADDHLD=2 DATAST=2 BUSTURN=5 CLKDIV=5 DATLAT=5 ACCMOD=A(0) */
  FSMC_BTR3 = 2u | (2u << 4) | (2u << 8) | (5u << 16) | (5u << 20) | (5u << 24);
  /* MBKEN | MWID=16b | WREN */
  FSMC_BCR3 = (1u << 0) | (1u << 4) | (1u << 12);
}

static void lcd_controller_init(void) {
  delay_ms(100);
  pin_set(GPIOD_BASE, PRIMER2_LCD_RST_PIN, 0);
  delay_ms(100);
  pin_set(GPIOD_BASE, PRIMER2_LCD_RST_PIN, 1);
  delay_ms(100);

  lcd_cmd(0x01); delay_ms(180);          /* SWRESET */
  lcd_cmd(0x10); delay_ms(100);          /* SLPIN   */
  lcd_cmd(0x11); delay_ms(150);          /* SLPOUT  */

  lcd_cmd(0xB1); lcd_dat(0x06); lcd_dat(0x03); lcd_dat(0x02); /* FRMCTR1 */
  lcd_cmd(0xB4); lcd_dat(0x03);                                /* INVCTR  */
  lcd_cmd(0xB6); lcd_dat(0x02); lcd_dat(0x0e);                 /* DISSET5 */
  lcd_cmd(0xF5); lcd_dat(0x1a);                                /* DISPCTRL*/
  lcd_cmd(0xC0); lcd_dat(0x02); lcd_dat(0x00);                 /* PWCTR1  */
  lcd_cmd(0xC1); lcd_dat(0x05);                                /* PWCTR2  */
  lcd_cmd(0xC2); lcd_dat(0x02); lcd_dat(0x02);                 /* PWCTR3  */
  lcd_cmd(0xC3); lcd_dat(0x01); lcd_dat(0x02);                 /* PWCTR4  */
  lcd_cmd(0xC4); lcd_dat(0x01); lcd_dat(0x02);                 /* PWCTR5  */
  lcd_cmd(0xC5); lcd_dat(0x47); lcd_dat(0x3a);                 /* VMCTR   */
  lcd_cmd(0xF2); lcd_dat(0x02);                                /* OSCADJ  */
  lcd_cmd(0xF6); lcd_dat(0x4c);                                /* DEFADJ  */
  lcd_cmd(0xF8); lcd_dat(0x06);                                /* LOAD    */

  static const uint8_t g1[] = {0x06,0x1c,0x1f,0x1f,0x18,0x13,0x06,0x03,0x03,0x04,0x07,0x07,0x00};
  static const uint8_t g2[] = {0x0a,0x14,0x1b,0x18,0x12,0x0e,0x02,0x01,0x00,0x01,0x08,0x07,0x00};
  lcd_cmd(0xE0); for (unsigned i = 0; i < sizeof(g1); i++) lcd_dat(g1[i]);
  lcd_cmd(0xE1); for (unsigned i = 0; i < sizeof(g2); i++) lcd_dat(g2[i]);

  /* MADCTL: CircleOS uses 0x70 (MX+MV, landscape). We want natural portrait
     128x160 (CASET=x, RASET=y), so no rotation, RGB order. */
  lcd_cmd(0x36); lcd_dat(0x00);
  lcd_cmd(0x3A); lcd_dat(0x55);          /* COLMOD: 16 bpp */
  lcd_cmd(0x35); lcd_dat(0x00);          /* TEON */
  lcd_cmd(0x29); delay_ms(20);           /* DISPON */
}

static void lcd_window(int x, int y, int w, int h) {
  int x1 = x + w - 1, y1 = y + h - 1;
  lcd_cmd(0x2A); lcd_dat(0); lcd_dat((uint8_t)x); lcd_dat(0); lcd_dat((uint8_t)x1);
  lcd_cmd(0x2B); lcd_dat(0); lcd_dat((uint8_t)y); lcd_dat(0); lcd_dat((uint8_t)y1);
  lcd_cmd(0x2C); /* RAMWR */
}
static inline void lcd_px(uint16_t c) { lcd_dat((uint8_t)(c >> 8)); lcd_dat((uint8_t)c); }

void board_lcd_fill_rect(int x, int y, int w, int h, uint16_t color) {
  if (x < 0 || y < 0 || w <= 0 || h <= 0) return;
  if (x + w > BOARD_LCD_W) w = BOARD_LCD_W - x;
  if (y + h > BOARD_LCD_H) h = BOARD_LCD_H - y;
  if (w <= 0 || h <= 0) return;
  lcd_window(x, y, w, h);
  for (int i = w * h; i > 0; i--) lcd_px(color);
}
void board_lcd_clear(uint16_t color) { board_lcd_fill_rect(0, 0, BOARD_LCD_W, BOARD_LCD_H, color); }
void board_lcd_pixel(int x, int y, uint16_t color) {
  if ((unsigned)x >= BOARD_LCD_W || (unsigned)y >= BOARD_LCD_H) return;
  lcd_window(x, y, 1, 1);
  lcd_px(color);
}
void board_lcd_present(void) { /* drawing goes straight to GRAM */ }

/* ---------------- joystick ---------------- */
/* Pressed shorts the line to the pulled-up common => reads HIGH. Inputs use
   internal pull-downs so an open contact reads LOW. */
static int joy_raw(void) {
  int m = 0;
  if (pin_get(GPIOE_BASE, PRIMER2_JOY_UP_PIN)) m |= BTN_UP;
  if (pin_get(GPIOE_BASE, PRIMER2_JOY_DOWN_PIN)) m |= BTN_DOWN;
  if (pin_get(GPIOE_BASE, PRIMER2_JOY_LEFT_PIN)) m |= BTN_LEFT;
  if (pin_get(GPIOE_BASE, PRIMER2_JOY_RIGHT_PIN)) m |= BTN_RIGHT;
  if (pin_get(GPIOA_BASE, PRIMER2_JOY_CENTER_PIN)) m |= BTN_CENTER;
  return m;
}

int board_input_wait(void) {
  static int prev;
  for (;;) {
    int cur = joy_raw();
    int edge = cur & ~prev;
    prev = cur;
    if (edge) {
      delay_ms(30); /* debounce */
      return edge;
    }
    delay_ms(10);
    /* heartbeat so we can see the firmware is alive */
    static uint32_t t;
    if (((++t) & 0x3f) == 0) GPIO_ODR(GPIOE_BASE) ^= (1u << PRIMER2_LED0_PIN);
  }
}

/* ---------------- audio (not wired yet) ---------------- */
void board_audio_open(unsigned rate, uint32_t total_frames) { (void)rate; (void)total_frames; }
void board_audio_write(const int16_t *s, size_t frames) { (void)s; (void)frames; }
void board_audio_close(void) {}

/* ---------------- storage: songs in flash ---------------- */
int board_storage_count(void) { return SONG_COUNT; }
const char *board_storage_name(int i) {
  return (i >= 0 && i < SONG_COUNT) ? song_table[i].name : "";
}
const char *board_storage_group(int i) {
  return (i >= 0 && i < SONG_COUNT) ? song_table[i].group : "";
}
int board_storage_load(int i, uint8_t *buf, size_t maxlen) {
  if (i < 0 || i >= SONG_COUNT) return -1;
  uint32_t len = song_table[i].len;
  if (len > maxlen) return -1;
  const uint8_t *src = &song_blob[song_table[i].off];
  for (uint32_t k = 0; k < len; k++) buf[k] = src[k]; /* must be RAM: driver writes loop counters */
  return (int)len;
}

/* ---------------- lifecycle ---------------- */
void board_log(const char *s) { (void)s; }

void board_init(void) {
  clock_init();
  /* GPIOA,B,D,E + AFIO clocks */
  RCC_APB2ENR |= (1u << 0) | (1u << 2) | (1u << 3) | (1u << 5) | (1u << 6);

  /* LEDs */
  pin_cfg(GPIOE_BASE, PRIMER2_LED0_PIN, CNF_OUT_PP_50);
  pin_cfg(GPIOE_BASE, PRIMER2_LED1_PIN, CNF_OUT_PP_50);
  pin_set(GPIOE_BASE, PRIMER2_LED1_PIN, 1); /* alive indicator */

  /* joystick inputs, pull-down */
  pin_cfg(GPIOE_BASE, PRIMER2_JOY_LEFT_PIN, CNF_IN_PULL);
  pin_cfg(GPIOE_BASE, PRIMER2_JOY_UP_PIN, CNF_IN_PULL);
  pin_cfg(GPIOE_BASE, PRIMER2_JOY_RIGHT_PIN, CNF_IN_PULL);
  pin_cfg(GPIOE_BASE, PRIMER2_JOY_DOWN_PIN, CNF_IN_PULL);
  pin_cfg(GPIOA_BASE, PRIMER2_JOY_CENTER_PIN, CNF_IN_PULL);
  GPIO_ODR(GPIOE_BASE) &= ~((1u << PRIMER2_JOY_LEFT_PIN) | (1u << PRIMER2_JOY_UP_PIN) |
                            (1u << PRIMER2_JOY_RIGHT_PIN) | (1u << PRIMER2_JOY_DOWN_PIN));
  GPIO_ODR(GPIOA_BASE) &= ~(1u << PRIMER2_JOY_CENTER_PIN);

  /* backlight PB8, active low -> drive LOW to turn on */
  pin_cfg(GPIOB_BASE, PRIMER2_BL_PIN, CNF_OUT_PP_50);
  pin_set(GPIOB_BASE, PRIMER2_BL_PIN, 0);

  fsmc_init();
  lcd_controller_init();
}
