/* pocket-fmdsp entry for the real STM32 Primer2 (no semihosting).
   FreeRTOS: a high-priority app/audio task and a low-priority LCD task, so a
   meter redraw can never starve the codec (audio always preempts drawing). */
#include "board.h"
#include "ui.h"

#ifdef PFM_RTOS
#include "FreeRTOS.h"
#include "task.h"

/* fully static allocation — no heap */
static StaticTask_t s_app_tcb, s_lcd_tcb, s_idle_tcb;
static StackType_t s_app_stack[768]; /* room for FatFs's deeper call chain */
static StackType_t s_lcd_stack[256];
static StackType_t s_idle_stack[configMINIMAL_STACK_SIZE];

void vApplicationGetIdleTaskMemory(StaticTask_t **tcb, StackType_t **stack,
                                   uint32_t *size) {
  *tcb = &s_idle_tcb;
  *stack = s_idle_stack;
  *size = configMINIMAL_STACK_SIZE;
}

static void app_task(void *arg) {
  (void)arg;
  ui_run();               /* menu + playback (high priority) */
  for (;;) vTaskDelay(1000);
}
static void lcd_task(void *arg) {
  (void)arg;
  ui_lcd_task();          /* meter drawing (lowest priority) */
}

int main(void) {
  board_init();
  ui_init();
  xTaskCreateStatic(app_task, "app", 768, NULL, 2, s_app_stack, &s_app_tcb);
  xTaskCreateStatic(lcd_task, "lcd", 256, NULL, 1, s_lcd_stack, &s_lcd_tcb);
  vTaskStartScheduler();
  for (;;) {
  }
}
#else
int main(void) {
  board_init();
  ui_run();
  for (;;) {
  }
}
#endif
