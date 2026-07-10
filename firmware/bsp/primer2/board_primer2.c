/*
 * Real STM32 Primer2 board backend.
 *
 * Register-level, no vendor SPL. The LCD path (FSMC bank NORSRAM3 @0x68000000,
 * GPIO chip-select, ST7732 controller, byte<<4 on the 16-bit bus) is taken from
 * the CircleOS 4.62 driver (Circle/OS/Hardware/Primer_1_2/lcd_spe.c), which is
 * the authoritative source for this panel.
 *
 * Clock: 12 MHz HSE -> PLL x6 -> 72 MHz.
 * Tracks live in flash (gen/songs.h); no SD driver yet. Audio plays through the
 * STW5094A codec (I2S3 + DMA2_Ch2), resampled from 55466 Hz to 48 kHz.
 */
#include "board.h"
#include "primer2_pins.h"
#include "songs.h"
#include "pfm/pfm_config.h"
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

#define RCC_APB1ENR REG32(0x4002101C)
#define TIM4_BASE  0x40000800u
#define TIM4_CR1   REG32(TIM4_BASE + 0x00)
#define TIM4_EGR   REG32(TIM4_BASE + 0x14)
#define TIM4_CCMR2 REG32(TIM4_BASE + 0x1C)
#define TIM4_CCER  REG32(TIM4_BASE + 0x20)
#define TIM4_PSC   REG32(TIM4_BASE + 0x28)
#define TIM4_ARR   REG32(TIM4_BASE + 0x2C)
#define TIM4_CCR3  REG32(TIM4_BASE + 0x3C)

#define AFIO_MAPR  REG32(0x40010004)
#define I2C2_BASE  0x40005800u
#define I2C2_CR1   REG32(I2C2_BASE + 0x00)
#define I2C2_CR2   REG32(I2C2_BASE + 0x04)
#define I2C2_DR    REG32(I2C2_BASE + 0x10)
#define I2C2_SR1   REG32(I2C2_BASE + 0x14)
#define I2C2_SR2   REG32(I2C2_BASE + 0x18)
#define I2C2_CCR   REG32(I2C2_BASE + 0x1C)
#define I2C2_TRISE REG32(I2C2_BASE + 0x20)
#define SPI3_BASE  0x40003C00u
#define SPI3_CR2      REG32(SPI3_BASE + 0x04)
#define SPI3_I2SCFGR  REG32(SPI3_BASE + 0x1C)
#define SPI3_I2SPR    REG32(SPI3_BASE + 0x20)
#define DMA2_BASE  0x40020400u
#define DMA2_CCR2   REG32(DMA2_BASE + 0x1C)
#define DMA2_CNDTR2 REG32(DMA2_BASE + 0x20)
#define DMA2_CPAR2  REG32(DMA2_BASE + 0x24)
#define DMA2_CMAR2  REG32(DMA2_BASE + 0x28)

#define CNF_AF_OD_50 0xFu /* alternate function open-drain, 50 MHz */

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

  /* MADCTL: portrait 128x160, rotated 180 deg (MX+MY) so the panel is upright
     for the Primer2's physical orientation. RGB order. */
  lcd_cmd(0x36); lcd_dat(0xC0);
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

/* Backlight: TIM4_CH3 PWM on PB8, exactly as CircleOS (LCD_BackLightConfig).
   The panel enable is active-low but a transistor inverts it, so the MCU pin is
   effectively active-high via PWM1/polarity-high. */
static void backlight_init(void) {
  pin_cfg(GPIOB_BASE, PRIMER2_BL_PIN, CNF_AF_PP_50); /* PB8 -> TIM4_CH3 AF */
  RCC_APB1ENR |= (1u << 2);                          /* TIM4 clock */
  TIM4_PSC = 0;
  TIM4_ARR = 0xFFFF;
  TIM4_CCR3 = 0xC000;         /* bright (CircleOS default ~0x2000; higher = brighter) */
  TIM4_CCMR2 = (6u << 4);     /* OC3M = PWM mode 1 */
  TIM4_CCER = (1u << 8);      /* CC3E enable, CC3P = 0 (active high) */
  TIM4_EGR = 1;              /* load registers (UG) */
  TIM4_CR1 = 1;              /* enable counter (CEN) */
}

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

/* ---------------- Audio: STW5094A codec via I2S3 (SPI3) + DMA2_Ch2 ----------
   Codec config and I2S/DMA setup follow CircleOS audio_spe.c. The player renders
   at PFM_MIX_RATE (55466 Hz); we linearly resample to the codec's 48 kHz and
   stream through a DMA ring. board_audio_write() blocks when the ring is full,
   which paces rendering to real time. Loudspeaker is enabled (CR6 bit 0x10);
   the headphone jack's switch mutes it when a plug is inserted. */

#define AUD_RING 1024                    /* stereo frames in the DMA ring */
static int16_t g_ring[AUD_RING * 2];     /* interleaved L,R @ ~48 kHz */
static unsigned g_wr;                    /* write cursor (in samples) */

/* 55466 -> 48000 resampler (source-driven, linear) */
#define RS_STEP (((uint32_t)PFM_MIX_RATE << 16) / 48000u)
static int16_t rs_fifo[8][2];
static int rs_cnt;
static uint32_t rs_pos;

static void i2c_codec_write(const uint8_t *cr, int n) {
  volatile uint32_t t;
  I2C2_CR1 |= (1u << 8);                              /* START */
  t = 200000; while (!(I2C2_SR1 & (1u << 0)) && --t) {} /* SB */
  I2C2_DR = 0xE2;                                     /* addr, write */
  t = 200000; while (!(I2C2_SR1 & (1u << 1)) && --t) {} /* ADDR */
  (void)I2C2_SR2;                                     /* clear ADDR */
  I2C2_DR = 0x00;                                     /* sub-address (reg 0) */
  t = 200000; while (!(I2C2_SR1 & (1u << 7)) && --t) {} /* TXE */
  for (int i = 0; i < n; i++) {
    I2C2_DR = cr[i];
    t = 200000; while (!(I2C2_SR1 & (1u << 7)) && --t) {} /* TXE */
  }
  t = 200000; while (!(I2C2_SR1 & (1u << 2)) && --t) {} /* BTF */
  I2C2_CR1 |= (1u << 9);                              /* STOP */
}

static void codec_init(void) {
  RCC_APB2ENR |= (1u << 0);   /* AFIO */
  RCC_APB1ENR |= (1u << 22);  /* I2C2 */
  pin_cfg(GPIOB_BASE, 10, CNF_AF_OD_50); /* SCL */
  pin_cfg(GPIOB_BASE, 11, CNF_AF_OD_50); /* SDA */
  I2C2_CR1 = 0;
  I2C2_CR2 = 36;              /* APB1 = 36 MHz */
  I2C2_CCR = 180;            /* 100 kHz standard mode */
  I2C2_TRISE = 37;
  I2C2_CR1 = 1;              /* enable */

  uint8_t cr[22] = {0};
  cr[0] = 0x00;
  cr[1] = 0x14;
  cr[4] = 0x6f;
  cr[5] = 0x17;
  cr[6] = 0x10 | 0x0c | 0x02; /* loudspeaker + headphone + SE, not muted */
  cr[7] = 0x04;               /* loudspeaker gain */
  cr[8] = 0x14;               /* HP gain L */
  cr[9] = 0x14;               /* HP gain R */
  cr[12] = 0x84;
  cr[13] = 89;
  cr[14] = 89;
  cr[16] = 0x08;
  cr[18] = 0x60;
  cr[21] = 0x61;              /* audio mode + power on */
  i2c_codec_write(cr, 22);
}

static void i2s_dma_init(void) {
  /* disable JTAG, keep SWD -> frees PB3 (I2S3_CK) and PA15 (I2S3_WS) */
  AFIO_MAPR = (AFIO_MAPR & ~(7u << 24)) | (2u << 24); /* SWJ_CFG = 010 */
  RCC_APB1ENR |= (1u << 15);  /* SPI3/I2S3 */
  RCC_AHBENR |= (1u << 1);    /* DMA2 */
  pin_cfg(GPIOB_BASE, 3, CNF_AF_PP_50);  /* I2S3_CK */
  pin_cfg(GPIOB_BASE, 5, CNF_AF_PP_50);  /* I2S3_SD */
  pin_cfg(GPIOA_BASE, 15, CNF_AF_PP_50); /* I2S3_WS */

  SPI3_I2SCFGR = 0;
  SPI3_I2SPR = 23 | (1u << 8);           /* I2SDIV=23, ODD=1 -> ~47.9 kHz @72MHz */
  /* I2SMOD | cfg=master-tx (10) | std=MSB/left-justified (01) | 16-bit */
  SPI3_I2SCFGR = (1u << 11) | (2u << 8) | (1u << 4);
  SPI3_CR2 = (1u << 1);                  /* TXDMAEN */

  DMA2_CCR2 = 0;
  DMA2_CPAR2 = SPI3_BASE + 0x0C;         /* &SPI3->DR */
  DMA2_CMAR2 = (uint32_t)g_ring;
  DMA2_CNDTR2 = AUD_RING * 2;
  /* DIR mem->periph, CIRC, MINC, PSIZE=16, MSIZE=16, PL=high */
  DMA2_CCR2 = (1u << 4) | (1u << 5) | (1u << 7) | (1u << 8) | (1u << 10) | (2u << 12);
  DMA2_CCR2 |= 1u;                       /* enable */
  SPI3_I2SCFGR |= (1u << 10);            /* I2SE */
}

static unsigned ring_read_pos(void) { return (AUD_RING * 2) - DMA2_CNDTR2; }

static void ring_push(int16_t l, int16_t r) {
  unsigned next = (g_wr + 2) % (AUD_RING * 2);
  while (next == (ring_read_pos() & ~1u)) { __asm__ volatile("nop"); } /* full: busy-wait on DMA */
  g_ring[g_wr] = l;
  g_ring[g_wr + 1] = r;
  g_wr = next;
}

static int16_t lerp(int16_t a, int16_t b, uint32_t f) {
  return (int16_t)(a + (((int)b - (int)a) * (int)f >> 16));
}

void board_audio_open(unsigned rate, uint32_t total_frames) {
  (void)rate; (void)total_frames;
  for (unsigned i = 0; i < AUD_RING * 2; i++) g_ring[i] = 0;
  g_wr = 0;
  rs_cnt = 0;
  rs_pos = 0;
  codec_init();
  i2s_dma_init();
}

void board_audio_write(const int16_t *src, size_t frames) {
  for (size_t i = 0; i < frames; i++) {
    if (rs_cnt >= 8) rs_cnt = 7; /* safety */
    rs_fifo[rs_cnt][0] = src[i * 2 + 0];
    rs_fifo[rs_cnt][1] = src[i * 2 + 1];
    rs_cnt++;
    while ((int)(rs_pos >> 16) + 1 < rs_cnt) {
      int idx = rs_pos >> 16;
      uint32_t f = rs_pos & 0xffff;
      ring_push(lerp(rs_fifo[idx][0], rs_fifo[idx + 1][0], f),
                lerp(rs_fifo[idx][1], rs_fifo[idx + 1][1], f));
      rs_pos += RS_STEP;
    }
    int consumed = rs_pos >> 16;
    if (consumed > 0) {
      for (int k = consumed; k < rs_cnt; k++) {
        rs_fifo[k - consumed][0] = rs_fifo[k][0];
        rs_fifo[k - consumed][1] = rs_fifo[k][1];
      }
      rs_cnt -= consumed;
      rs_pos &= 0xffff;
    }
  }
}

void board_audio_close(void) {
  /* let the ring drain, then stop and mute */
  for (int guard = 0; guard < 200000; guard++) {
    unsigned rd = ring_read_pos() & ~1u;
    if (rd == g_wr) break;
    __asm__ volatile("nop");
  }
  SPI3_I2SCFGR &= ~(1u << 10); /* I2SE off */
  DMA2_CCR2 = 0;
  uint8_t mute[22] = {0};
  mute[6] = 0x20; /* MUT */
  i2c_codec_write(mute, 22);
}

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

  backlight_init(); /* TIM4 PWM on PB8 */

  fsmc_init();
  lcd_controller_init();
}
