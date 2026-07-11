#ifndef PFM_UI_H
#define PFM_UI_H
/* Track-list GUI: list songs from storage, navigate with the joystick, play the
   selected one. Renders via the board LCD HAL; drives the player via board audio. */
void ui_init(void);     /* create shared resources (LCD mutex); call before tasks */
void ui_run(void);      /* app/audio task: menu + playback loop */
void ui_sd_task(void);  /* highest-priority task: SD file reads (RTOS only) */
void ui_set_task_handles(void *sd, void *app); /* give the UI the sd + app task handles */
void ui_lcd_task(void); /* low-priority task: draws the playback meter (RTOS only) */
#endif
