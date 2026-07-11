#ifndef PFM_UI_H
#define PFM_UI_H
/* Track-list GUI: list songs from storage, navigate with the joystick, play the
   selected one. Renders via the board LCD HAL; drives the player via board audio. */
void ui_init(void);     /* create shared resources (LCD mutex); call before tasks */
void ui_run(void);      /* UI task: input + page management (no rendering) */
void ui_audio_task(void);        /* highest-prio task: OPNA render -> ring (RTOS only) */
void ui_set_audio_handle(void *h); /* give the UI the OPNA task handle for suspend/resume */
void ui_lcd_task(void); /* low-priority task: draws the playback meter (RTOS only) */
#endif
