#ifndef PFM_PRIMER2_PINS_H
#define PFM_PRIMER2_PINS_H
/*
 * STM32 Primer2 (Raisonance, rev 1.1) pin map — from docs/schema_stm32_primer2_1_2.pdf.
 * MCU: STM32F103VET6 (LQFP100), 12 MHz HSE -> 72 MHz. Used by the real-hardware
 * board backend (bsp/primer2/board_primer2.c). The QEMU/sim backend does not use
 * these — it talks to the host via semihosting.
 *
 * LCD: 8-bit 8080 parallel via FSMC bank1 (NE1), RS on address line A16.
 *   The panel is 128x160 colour; controller behind an ST7735-class 8080 iface.
 *   FSMC_D0..D7 : PD14 PD15 PD0 PD1 PE7 PE8 PE9 PE10   (DB0..DB7)
 *   FSMC_NWE=PD5 -> LCD_/WR
 *   FSMC_NOE=PD4 -> LCD_/RD
 *   FSMC_NE1=PD7 -> LCD_/CS
 *   FSMC_A16=PD11 -> LCD_RS (0=command, 1=data)
 *   LCD_/RST : GPIO (backlight BACKLIGHT_/EN active-low GPIO)
 *
 * Joystick (SW1 TPA-511G): common pulled high; each direction on a GPIO.
 *   UP=PE3  LEFT=PE2  RIGHT=PE4  DOWN=PE5  CENTER/PBUTTON=PA8
 *
 * LEDs: LED0=PE0  LED1=PE1   (green, active high via 330R)
 * microSD (J4): SDIO 4-bit. D0=PC8 D1=PC9 D2=PC10 D3=PC11 CK=PC12 CMD=PD2
 * Audio: STW5094A codec. I2S2: WS=PB12 CK=PB13 SD=PB15 (MISO PB14); MCLK 12MHz.
 *        Control via I2C1. Speaker KDMG10008C + headphone jack.
 * MEMS: LIS3LV02 accelerometer on SPI1 (SCK=PA5 MISO=PA6 MOSI=PA7, CS=CS_MEMS GPIO).
 */

/* Joystick GPIOs (port E except center on port A). */
#define PRIMER2_JOY_UP_PORT     GPIOE
#define PRIMER2_JOY_UP_PIN      3
#define PRIMER2_JOY_LEFT_PIN    2
#define PRIMER2_JOY_RIGHT_PIN   4
#define PRIMER2_JOY_DOWN_PIN    5
#define PRIMER2_JOY_CENTER_PORT GPIOA
#define PRIMER2_JOY_CENTER_PIN  8

#define PRIMER2_LED0_PIN        0 /* GPIOE */
#define PRIMER2_LED1_PIN        1 /* GPIOE */

/* LCD FSMC bank1/NE1: command at A16=0, data at A16=1. */
#define PRIMER2_LCD_BASE        0x60000000u          /* FSMC NE1 */
#define PRIMER2_LCD_REG         (*(volatile uint16_t *)0x60000000u) /* RS=0 (cmd) */
#define PRIMER2_LCD_DAT         (*(volatile uint16_t *)0x60020000u) /* A16=1 -> RS=1 (data) */
#define PRIMER2_LCD_W           128
#define PRIMER2_LCD_H           160

#endif /* PFM_PRIMER2_PINS_H */
