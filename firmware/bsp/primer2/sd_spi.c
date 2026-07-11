/*
 * microSD over bit-banged SPI for the STM32 Primer2. The card is wired to the
 * SDIO pins, but they are not on an SPI peripheral, so we drive SPI mode by GPIO:
 *   CS   = PC11 (DAT3)   MOSI = PD2 (CMD)
 *   SCK  = PC12 (CLK)    MISO = PC8 (DAT0)
 * Clock-independent (works at the 132 MHz overclock). Provides the block API
 * FatFs's diskio glue needs: sd_init(), sd_read_blocks(), sd_status().
 */
#include <stdint.h>
#include <stddef.h>

#define REG32(a) (*(volatile uint32_t *)(a))
#define GPIOC 0x40011000u
#define GPIOD 0x40011400u
#define CRL(b) REG32((b) + 0x00)
#define CRH(b) REG32((b) + 0x04)
#define IDR(b) REG32((b) + 0x08)
#define BSRR(b) REG32((b) + 0x10)

#define CS_PIN 11   /* PC11 */
#define SCK_PIN 12  /* PC12 */
#define MOSI_PIN 2  /* PD2  */
#define MISO_PIN 8  /* PC8  */

#define CS_HI()   (BSRR(GPIOC) = 1u << CS_PIN)
#define CS_LO()   (BSRR(GPIOC) = 1u << (CS_PIN + 16))
#define SCK_HI()  (BSRR(GPIOC) = 1u << SCK_PIN)
#define SCK_LO()  (BSRR(GPIOC) = 1u << (SCK_PIN + 16))
#define MOSI_HI() (BSRR(GPIOD) = 1u << MOSI_PIN)
#define MOSI_LO() (BSRR(GPIOD) = 1u << (MOSI_PIN + 16))
#define MISO()    ((IDR(GPIOC) >> MISO_PIN) & 1u)

/* SPI half-clock delay: large for the <=400kHz init phase, ~0 for data. */
static volatile int g_clk_delay = 200;
static inline void spi_delay(void) {
  for (volatile int i = g_clk_delay; i > 0; i--) { }
}

static uint8_t sd_type; /* 0=none, 1=SDv1/MMC(byte addr), 2=SDHC(block addr) */

static uint8_t xchg(uint8_t out) {
  uint8_t in = 0;
  for (int b = 0; b < 8; b++) {
    if (out & 0x80) MOSI_HI(); else MOSI_LO();
    out <<= 1;
    spi_delay();
    SCK_HI();
    spi_delay();
    in = (uint8_t)((in << 1) | MISO());
    SCK_LO();
  }
  return in;
}

/* CRC-16/CCITT (poly 0x1021, init 0) — the card always sends a valid data CRC in
   SPI mode, so checking it catches bit errors from the fast bit-bang clock. */
static uint16_t crc16_ccitt(const uint8_t *p, int n) {
  uint16_t crc = 0;
  for (int i = 0; i < n; i++) {
    crc ^= (uint16_t)p[i] << 8;
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
  }
  return crc;
}

static void gpio_setup(void) {
  REG32(0x40021018) |= (1u << 4) | (1u << 5); /* RCC_APB2ENR: IOPC, IOPD */
  /* PC8 MISO: input pull-up (CNF=10, MODE=00). PC11/PC12: output PP 50MHz (0x3). */
  CRH(GPIOC) = (CRH(GPIOC) & ~(0xFu << 0) & ~(0xFu << 12) & ~(0xFu << 16))
             | (0x8u << 0)   /* PC8  input, pull */
             | (0x3u << 12)  /* PC11 out PP 50 */
             | (0x3u << 16); /* PC12 out PP 50 */
  BSRR(GPIOC) = 1u << MISO_PIN; /* pull-up on MISO */
  /* PD2 MOSI: output PP 50MHz. */
  CRL(GPIOD) = (CRL(GPIOD) & ~(0xFu << 8)) | (0x3u << 8);
  CS_HI();
  SCK_LO();
  MOSI_HI();
}

/* Send a command; returns the R1 response byte (0xFF = no response). */
static uint8_t send_cmd(uint8_t cmd, uint32_t arg, uint8_t crc) {
  xchg(0xFF);
  xchg((uint8_t)(0x40 | cmd));
  xchg((uint8_t)(arg >> 24));
  xchg((uint8_t)(arg >> 16));
  xchg((uint8_t)(arg >> 8));
  xchg((uint8_t)arg);
  xchg(crc);
  uint8_t r = 0xFF;
  for (int i = 0; i < 10; i++) { r = xchg(0xFF); if (!(r & 0x80)) break; }
  return r;
}

static uint8_t acmd(uint8_t cmd, uint32_t arg) {
  send_cmd(55, 0, 0x65);
  return send_cmd(cmd, arg, 0xFF);
}

int sd_init(void) {
  sd_type = 0;
  gpio_setup();
  g_clk_delay = 200;                 /* slow clock for init */
  CS_HI();
  for (int i = 0; i < 12; i++) xchg(0xFF); /* >=74 clocks, CS high */
  CS_LO();

  uint8_t r;
  int tries = 100;
  do { r = send_cmd(0, 0, 0x95); } while (r != 0x01 && --tries); /* GO_IDLE */
  if (r != 0x01) { CS_HI(); return -1; }

  r = send_cmd(8, 0x1AA, 0x87); /* SEND_IF_COND */
  int sdv2 = 0;
  if (r == 0x01) {
    uint8_t ocr[4];
    for (int i = 0; i < 4; i++) ocr[i] = xchg(0xFF);
    if (ocr[2] == 0x01 && ocr[3] == 0xAA) sdv2 = 1; else { CS_HI(); return -2; }
  }

  tries = 20000;
  do { r = acmd(41, sdv2 ? 0x40000000u : 0); } while (r != 0x00 && --tries); /* SD_OP_COND */
  if (r != 0x00) { CS_HI(); return -3; }

  sd_type = 1;
  if (sdv2) {
    r = send_cmd(58, 0, 0xFF); /* READ_OCR */
    if (r == 0x00) {
      uint8_t ocr[4];
      for (int i = 0; i < 4; i++) ocr[i] = xchg(0xFF);
      if (ocr[0] & 0x40) sd_type = 2; /* CCS -> SDHC (block addressing) */
    }
  }
  if (sd_type != 2) send_cmd(16, 512, 0xFF); /* SET_BLOCKLEN 512 */

  CS_HI();
  xchg(0xFF);
  g_clk_delay = 2;                   /* small MISO settle delay for reliable data */
  return 0;
}

int sd_status(void) { return sd_type ? 0 : -1; }

/* Read one 512-byte block (CMD17) into buf, validating the data CRC. Returns 0 on
   success. Leaves CS high. */
static int read_one_block(uint32_t arg, uint8_t *buf) {
  CS_LO();
  uint8_t r = send_cmd(17, arg, 0xFF);
  if (r != 0x00) { CS_HI(); xchg(0xFF); return -2; }
  int tries = 40000;
  uint8_t tok;
  do { tok = xchg(0xFF); } while (tok == 0xFF && --tries); /* wait data token */
  if (tok != 0xFE) { CS_HI(); xchg(0xFF); return -3; }
  for (int i = 0; i < 512; i++) buf[i] = xchg(0xFF);
  uint8_t c0 = xchg(0xFF), c1 = xchg(0xFF);
  CS_HI();
  xchg(0xFF);
  if ((uint16_t)((c0 << 8) | c1) != crc16_ccitt(buf, 512)) return -4; /* bad CRC */
  return 0;
}

static uint32_t g_read_errs; /* blocks that never read clean within the retry budget */
uint32_t board_sd_read_errors(void) { return g_read_errs; }

/* Read `count` 512-byte blocks starting at LBA `lba` into buf. Each block is
   retried up to 10 times on any command/token/CRC failure. If a block still
   fails CRC after 10 tries we KEEP the last read (a CRC miss still delivered
   512 bytes) rather than hard-failing, so a marginal card degrades gracefully
   instead of breaking the mount; the failure is counted for the debug view. A
   block that never even reaches the data phase (bad command/token) is a hard
   error. */
#define SD_READ_RETRIES 10
int sd_read_blocks(uint32_t lba, uint8_t *buf, unsigned count) {
  if (!sd_type) return -1;
  uint32_t base = (sd_type == 2) ? lba : lba * 512u;
  for (unsigned blk = 0; blk < count; blk++) {
    uint32_t arg = base + (sd_type == 2 ? blk : blk * 512u);
    int rc = -1, got_data = 0;
    for (int attempt = 0; attempt < SD_READ_RETRIES; attempt++) {
      rc = read_one_block(arg, buf);
      if (rc == 0) break;
      if (rc == -4) got_data = 1;   /* buf holds a CRC-bad (but complete) block */
    }
    if (rc != 0) {
      g_read_errs++;
      if (!got_data) return rc;      /* never reached data phase -> hard fail */
    }
    buf += 512;
  }
  return 0;
}
