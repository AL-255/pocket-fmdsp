#ifndef PFM_PRIMER2_PINS_H
#define PFM_PRIMER2_PINS_H
/*
 * STM32 Primer2 (Raisonance rev 1.1) pin map, read off
 * docs/schema_stm32_primer2_1_2.pdf sheet 2. MCU: STM32F103VET6 (LQFP100),
 * 12 MHz HSE -> 72 MHz.  LCD is 128x160 24-bit colour (per the user manual).
 *
 * LCD: 8-bit 8080 bus on FSMC bank1 / NE1, RS on FSMC_A16.
 *   IMPORTANT: the panel's DB0..DB7 are wired to FSMC_D4..D11 (PE7..PE14),
 *   *not* D0..D7. So the FSMC runs 16-bit wide and every LCD byte is written
 *   as (byte << 4). Only PE7..PE14 are muxed to FSMC AF; FSMC_D0..D3 / D12..D15
 *   stay as GPIO (PD0/PD1 are CAN on this board).
 *
 *   FSMC_D4..D11 = PE7 PE8 PE9 PE10 PE11 PE12 PE13 PE14   -> LCD_D0..LCD_D7
 *   PD4  = FSMC_NOE   -> LCD_/RD
 *   PD5  = FSMC_NWE   -> LCD_/WR
 *   PD6  = GPIO out   -> LCD_/RST
 *   PD7  = FSMC_NE1   -> LCD_/CS
 *   PD11 = FSMC_A16   -> LCD_RS  (0 = command, 1 = data)
 *   PB8  = GPIO out   -> BACKLIGHT_/EN  (active low; pulled high = off)
 *
 *   In 16-bit FSMC mode the address bus is shifted by one, so A16 = CPU addr
 *   bit 17: command @ 0x60000000, data @ 0x60020000.
 *
 * Joystick (SW1): common pulled to VCCS via 10k; a pressed direction shorts to
 * common => reads HIGH. Configure inputs with internal pull-down.
 *   PE3 = LEFT   PE4 = UP   PE5 = RIGHT   PE6 = DOWN   PA8 = CENTER (PBUTTON)
 *   (PE2 = CS_MEMS, not the joystick.)
 *
 * LEDs: PE0 = LED0, PE1 = LED1 (active high). Verified working via blinky.
 * microSD: SDIO 4-bit  D0=PC8 D1=PC9 D2=PC10 D3=PC11 CK=PC12 CMD=PD2
 * Audio  : STW5094A codec. I2S2 (SPI2): WS=PB12 CK=PB13 SD=PB15.
 *          Control via I2C2: SCL=PB10 SDA=PB11.
 */

#define PRIMER2_LCD_W 128
#define PRIMER2_LCD_H 160

/* FSMC bank1/NE1: RS=A16 -> bit17 of the CPU address in 16-bit mode. */
#define PRIMER2_LCD_CMD_ADDR 0x60000000u
#define PRIMER2_LCD_DAT_ADDR 0x60020000u

/* Joystick / button pins (GPIOE). Verified against the physical directions:
   PE3=UP, PE4=DOWN, PE5=RIGHT, PE6=LEFT. */
#define PRIMER2_JOY_UP_PIN     3 /* GPIOE */
#define PRIMER2_JOY_DOWN_PIN   4 /* GPIOE */
#define PRIMER2_JOY_RIGHT_PIN  5 /* GPIOE */
#define PRIMER2_JOY_LEFT_PIN   6 /* GPIOE */
#define PRIMER2_JOY_CENTER_PIN 8 /* GPIOA (PBUTTON) */

#define PRIMER2_LED0_PIN 0 /* GPIOE */
#define PRIMER2_LED1_PIN 1 /* GPIOE */
#define PRIMER2_BL_PIN   8 /* GPIOB, active low */
#define PRIMER2_LCD_RST_PIN 6 /* GPIOD */

#endif /* PFM_PRIMER2_PINS_H */
