/*
 * Stage-1 hardware bring-up: alternately blink the two Primer2 LEDs
 * (LED0=PE0, LED1=PE1 per the schematic). No clock init — runs on the default
 * 8 MHz HSI, so a visible blink means: build -> flash -> Cortex-M3 executes our
 * vector table + startup + GPIO. Deliberately minimal to de-risk the flash path
 * before adding the clock tree, FSMC LCD, etc.
 */
#include <stdint.h>

#define RCC_APB2ENR (*(volatile uint32_t *)0x40021018u)
#define GPIOE_CRL   (*(volatile uint32_t *)0x40011800u)
#define GPIOE_BSRR  (*(volatile uint32_t *)0x40011810u)

#define RCC_IOPEEN  (1u << 6) /* GPIOE clock enable */

static void delay(volatile uint32_t n) {
  while (n--) __asm__ volatile("nop");
}

int main(void) {
  RCC_APB2ENR |= RCC_IOPEEN;
  /* PE0, PE1 -> output push-pull, 2 MHz  (MODE=10, CNF=00 => 0x2 per pin) */
  GPIOE_CRL = (GPIOE_CRL & ~0xffu) | 0x22u;

  for (;;) {
    /* PE0 on, PE1 off */
    GPIOE_BSRR = (1u << 0) | (1u << (16 + 1));
    delay(600000);
    /* PE1 on, PE0 off */
    GPIOE_BSRR = (1u << 1) | (1u << (16 + 0));
    delay(600000);
  }
}
