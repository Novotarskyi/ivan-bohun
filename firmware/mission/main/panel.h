#pragma once
#include "lvgl.h"

/* The 4.3" panel (Waveshare ESP32-S3-Touch-LCD-4.3B): 800x480 RGB565 parallel
 * + GT911 capacitive touch + CH422G IO expander (backlight, LCD/touch resets).
 * Pin map transcribed from the vendor's own ESP-IDF demos:
 *   github.com/waveshareteam/ESP32-S3-Touch-LCD-4.3B
 *     examples/ESP-IDF/05_IO_Test/lib/LCD/LCD.h          (RGB pins, 800x480 timings)
 *     examples/ESP-IDF/08_lvgl_Porting/main/waveshare_rgb_lcd_port.h (I2C 8/9, 16 MHz PCLK)
 *     examples/ESP-IDF/05_IO_Test/lib/CH422G/CH422G.h    (expander regs + line roles)
 */
#define PANEL_H_RES 800
#define PANEL_V_RES 480

/* brings up expander -> resets -> backlight -> RGB panel -> touch -> LVGL.
 * returns the LVGL display for the UI to build on. */
lv_display_t *panel_init(void);

/* backlight via CH422G IO2 - on/off only (no PWM through the expander).
 * resets (IO1/IO3) stay asserted high either way; touch keeps working dark. */
void panel_backlight(bool on);
