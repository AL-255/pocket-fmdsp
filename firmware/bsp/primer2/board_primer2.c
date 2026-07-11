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
#include "pfm/pfm_prof.h"
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

/* ---------------- clock + delay ----------------
   Primer2 has a 12 MHz HSE crystal. The STM32F103's rated max is 72 MHz, but the
   FM render is CPU-bound, so we overclock the core (HCLK = SYSCLK) to 96 MHz for
   headroom. To keep peripherals within spec we divide the buses down: APB1 = /4
   = 24 MHz, APB2 = /2 = 48 MHz. Flash stays at 2 wait states + prefetch (out of
   spec above 72 MHz, but the prefetch buffer covers it at 96 MHz on this part).

   The I2S audio bit clock is derived from SYSCLK, so the codec sample rate moves
   with it; I2S_FS (below) is computed from SYSCLK_HZ and the resampler targets
   that exact value, so pitch stays correct at any of these clock choices. To
   change the overclock, edit PLL_MUL only (and re-check I2S_I2SDIV for ~48 kHz). */
#define HSE_HZ     12000000u
#define PLL_MUL    12u                       /* overclock search: x12 -> 144 MHz (200% of rated) */
#define SYSCLK_HZ  (HSE_HZ * PLL_MUL)       /* 144 MHz */
#define APB1_HZ    (SYSCLK_HZ / 4u)         /* 36 MHz (PPRE1 = /4) */

static void clock_init(void) {
  RCC_CR |= (1u << 16);                        /* HSEON */
  for (volatile int t = 0; t < 400000; t++)
    if (RCC_CR & (1u << 17)) break;            /* HSERDY */
  FLASH_ACR = 0x12;                            /* prefetch + 2 wait states */
  RCC_CR &= ~(1u << 24);                       /* PLL off while configuring */
  /* PLLSRC=HSE, PLLXTPRE=0, PLLMUL=(PLL_MUL-2), PPRE1=/4 (101), PPRE2=/2 (100) */
  RCC_CFGR = (1u << 16) | ((PLL_MUL - 2u) << 18) | (0x5u << 8) | (0x4u << 11);
  RCC_CR |= (1u << 24);                        /* PLLON */
  for (volatile int t = 0; t < 400000; t++)
    if (RCC_CR & (1u << 25)) break;            /* PLLRDY */
  RCC_CFGR |= 0x2u;                            /* SW = PLL */
  for (volatile int t = 0; t < 400000; t++)
    if ((RCC_CFGR & 0xCu) == 0x8u) break;      /* SWS = PLL */
}

static void delay_ms(uint32_t ms) {
  SYST_RVR = (SYSCLK_HZ / 1000u) - 1u;
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

int board_input_poll(void) { return joy_raw(); }

/* DWT cycle counter (Cortex-M3 core debug) for the render perf meter. */
#define DEMCR     REG32(0xE000EDFC)
#define DWT_CTRL  REG32(0xE0001000)
#define DWT_CYCCNT REG32(0xE0001004)
uint32_t board_cycles(void) { return DWT_CYCCNT; }
uint32_t board_cpu_hz(void) { return SYSCLK_HZ; }

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
   The circular DMA buffer IS the FIFO between the emulator and the codec:
     - producer: pfm_player_render() -> board_audio_write() pushes OPNA frames
       1:1 (no resample); ring_push() stalls (busy-waits) on backpressure when
       the FIFO is full, so the OPNA runs as fast as it can but no faster.
     - consumer: DMA2 pops the FIFO at a rock-steady rate set purely by the I2S
       bit clock (I2S_FS), feeding the codec at exactly that rate.
   We therefore program the I2S divider so I2S_FS matches the OPNA's native rate
   (PFM_MIX_RATE = 55467 Hz) as closely as the divider allows -> no resampling.
   Output is routed to the headphone jack only (CR6 bits 0x0c = PHL/PHR); the
   loudspeaker (bit 0x10) is off, as this board has no HP-detect line. */

#define AUD_RING 2048                    /* stereo frames in the DMA FIFO (~37 ms) */
static int16_t g_ring[AUD_RING * 2];     /* interleaved L,R @ I2S_FS */
static unsigned g_wr;                    /* write cursor (in samples) */

/* I2S Fs = SYSCLK / (32 * (2*I2SDIV + ODD)). Pick the divider closest to the
   OPNA rate: at 96 MHz, I2SDIV=27, ODD=0 -> 96e6/(32*54) = 55556 Hz, i.e.
   PFM_MIX_RATE + 0.16% (~3 cents, inaudible). So the codec consumes exactly
   what the emulator produces, 1:1. */
#define I2S_I2SDIV 40u
#define I2S_ODD    1u   /* 2*40+1 = 81 -> 144e6/(32*81) = 55555.6 Hz (= OPNA rate, exact) */
#define I2S_FS (SYSCLK_HZ / (32u * (2u * I2S_I2SDIV + I2S_ODD))) /* 55556 Hz */

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

/* Write a single codec register (sub-address = reg). */
static void i2c_codec_write_reg(uint8_t reg, uint8_t val) {
  volatile uint32_t t;
  I2C2_CR1 |= (1u << 8);
  t = 200000; while (!(I2C2_SR1 & (1u << 0)) && --t) {}
  I2C2_DR = 0xE2;
  t = 200000; while (!(I2C2_SR1 & (1u << 1)) && --t) {}
  (void)I2C2_SR2;
  I2C2_DR = reg;
  t = 200000; while (!(I2C2_SR1 & (1u << 7)) && --t) {}
  I2C2_DR = val;
  t = 200000; while (!(I2C2_SR1 & (1u << 7)) && --t) {}
  t = 200000; while (!(I2C2_SR1 & (1u << 2)) && --t) {}
  I2C2_CR1 |= (1u << 9);
}

/* Volume 0..BOARD_VOL_MAX -> STW5094A headphone gain (CR8/CR9). */
void board_audio_set_volume(int level) {
  if (level < 0) level = 0;
  if (level > BOARD_VOL_MAX) level = BOARD_VOL_MAX;
  uint8_t g = (uint8_t)(level * 3); /* 0..30, ~0x14 at the mid default */
  i2c_codec_write_reg(8, g);
  i2c_codec_write_reg(9, g);
}

static void codec_init(void) {
  RCC_APB2ENR |= (1u << 0);   /* AFIO */
  RCC_APB1ENR |= (1u << 22);  /* I2C2 */
  pin_cfg(GPIOB_BASE, 10, CNF_AF_OD_50); /* SCL */
  pin_cfg(GPIOB_BASE, 11, CNF_AF_OD_50); /* SDA */
  I2C2_CR1 = 0;
  I2C2_CR2 = APB1_HZ / 1000000u;             /* APB1 clock in MHz (=24) */
  I2C2_CCR = APB1_HZ / (2u * 100000u);       /* 100 kHz standard mode (=120) */
  I2C2_TRISE = APB1_HZ / 1000000u + 1u;      /* (=25) */
  I2C2_CR1 = 1;              /* enable */

  uint8_t cr[22] = {0};
  cr[0] = 0x00;
  cr[1] = 0x14;
  cr[4] = 0x6f;
  cr[5] = 0x17;
  cr[6] = 0x0c | 0x02;        /* headphone (PHL/PHR) + SE, loudspeaker off, not muted */
  cr[7] = 0x04;               /* output gain */
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
  SPI3_I2SPR = I2S_I2SDIV | (I2S_ODD << 8); /* -> I2S_FS = OPNA rate */
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

void board_audio_open(unsigned rate, uint32_t total_frames) {
  (void)rate; (void)total_frames;
  for (unsigned i = 0; i < AUD_RING * 2; i++) g_ring[i] = 0;
  g_wr = 0;
  codec_init();
  i2s_dma_init();
}

/* Push OPNA frames into the DMA FIFO 1:1 (no resample). Copies in contiguous
   runs, reading the DMA position only once per run instead of per frame. In
   SRAM (.ramfunc): the copy loop must not run from the under-spec flash. */
PFM_HOT void board_audio_write(const int16_t *src, size_t frames) {
  uint32_t prof_t0 = pfm_prof_begin();
  const unsigned size = AUD_RING * 2;         /* samples in the ring */
  size_t total = frames * 2, done = 0;
  while (done < total) {
    unsigned rd = ring_read_pos() & ~1u;      /* DMA read cursor (one AHB read) */
    unsigned wr = g_wr;
    unsigned freesmp = (rd - wr - 2u) & (size - 1u); /* free samples, 1-frame gap */
    if (freesmp == 0) { __asm__ volatile("nop"); continue; } /* full: wait for DMA */
    unsigned run = size - wr;                 /* contiguous run to ring end */
    if (run > freesmp) run = freesmp;
    unsigned rem = (unsigned)(total - done);
    if (run > rem) run = rem;
    for (unsigned k = 0; k < run; k++) g_ring[wr + k] = src[done + k];
    wr += run;
    if (wr >= size) wr -= size;
    g_wr = wr;
    done += run;
  }
  pfm_prof_end(PFM_PROF_OUTPUT, prof_t0);
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
  /* enable DWT cycle counter (perf meter) */
  DEMCR |= (1u << 24);      /* TRCENA */
  DWT_CYCCNT = 0;
  DWT_CTRL |= 1u;           /* CYCCNTENA */
  pfm_prof_clock = board_cycles; /* enable per-task CPU profiling */
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
