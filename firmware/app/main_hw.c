/* pocket-fmdsp entry for the real STM32 Primer2 (no semihosting).
   FreeRTOS: a high-priority app/audio task and a low-priority LCD task, so a
   meter redraw can never starve the codec (audio always preempts drawing). */
#include "board.h"
#include "ui.h"

#ifdef PFM_RTOS
#include "FreeRTOS.h"
#include "task.h"

/* fully static allocation — no heap. Priorities: audio(3) > ui(2) > lcd(1) > idle.
   The audio task only renders, so nothing (SD load, input, drawing) can stall it. */
static StaticTask_t s_audio_tcb, s_app_tcb, s_lcd_tcb, s_idle_tcb;
static StackType_t s_audio_stack[512]; /* OPNA render chain */
static StackType_t s_app_stack[768];   /* UI + FatFs (FIL carries a 512B sector buffer) */
static StackType_t s_lcd_stack[256];
static StackType_t s_idle_stack[configMINIMAL_STACK_SIZE];

void vApplicationGetIdleTaskMemory(StaticTask_t **tcb, StackType_t **stack,
                                   uint32_t *size) {
  *tcb = &s_idle_tcb;
  *stack = s_idle_stack;
  *size = configMINIMAL_STACK_SIZE;
}

static void audio_task(void *arg) {
  (void)arg;
  ui_audio_task();        /* OPNA render -> ring (highest priority) */
}
static void app_task(void *arg) {
  (void)arg;
  ui_run();               /* input + pages */
  for (;;) vTaskDelay(1000);
}
static void lcd_task(void *arg) {
  (void)arg;
  ui_lcd_task();          /* meter drawing (lowest priority) */
}

int main(void) {
  board_init();
  ui_init();
  TaskHandle_t audio_h =
    xTaskCreateStatic(audio_task, "opna", 512, NULL, 3, s_audio_stack, &s_audio_tcb);
  ui_set_audio_handle(audio_h);
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
