/*
 * pocket-fmdsp firmware entry: init the board, run the track-list GUI.
 * On the sim (QEMU) the board HAL is semihosting-backed, so this produces LCD
 * screenshots and WAV audio the same way it would drive the real Primer2.
 */
#include "board.h"
#include "ui.h"
#include "semihosting.h"

int main(void) {
  sh_write0("\n=== pocket-fmdsp (STM32 Primer2 / QEMU) ===\n");
  board_init();
  ui_run();
  sh_write0("[sim] finished\n");
  sh_exit(0);
  return 0;
}
