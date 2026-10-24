#ifndef STATUS_LED_H
#define STATUS_LED_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/*
 * Status LED states: Rich Perceptual Palette
 * Calibrated for naked WS2812B LEDs with optical white-balancing
 * (compensates for eye sensitivity to green and bare-die separation)
 */
typedef enum {
    LED_COLOR_BLACK = 0,
    LED_COLOR_RED,      /* Error / Fault (Pure Red) */
    LED_COLOR_GREEN,    /* Ready / Idle (Balanced Green) */
    LED_COLOR_BLUE,     /* Connecting / Staging (Pure Blue) */
    LED_COLOR_YELLOW,   /* Booting / Initializing (Perceptual Yellow) */
    LED_COLOR_CYAN,     /* Emulation Active / Inserted (Perceptual Cyan: G suppressed, B dominant) */
    LED_COLOR_MAGENTA,  /* Upload in progress (Perceptual Magenta / Violet) */
    LED_COLOR_WHITE,    /* Activity Flash / POST (Balanced D65 White) */
    LED_COLOR_ORANGE    /* Target Power / Reset event */
} status_led_color_t;

esp_err_t status_led_init(void);
void status_led_set_color(status_led_color_t color);
void status_led_set_rgb(uint8_t r, uint8_t g, uint8_t b);
void status_led_activity_flash(status_led_color_t flash_color);

#endif /* STATUS_LED_H */
