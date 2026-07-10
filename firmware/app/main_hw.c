/* pocket-fmdsp entry for the real STM32 Primer2 (no semihosting). */
#include "board.h"
#include "ui.h"

int main(void) {
  board_init();
  ui_run();
  for (;;) {
  }
}
