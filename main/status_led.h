#ifndef STATUS_LED_H
#define STATUS_LED_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Status LED states: Single Pure Colors for maximum clarity without RGB blending artifacts */
typedef enum {
    LED_COLOR_BLACK = 0,
    LED_COLOR_RED,      /* Error / Fault */
    LED_COLOR_GREEN,    /* Ready / Idle (Pure Green) */
    LED_COLOR_BLUE,     /* Emulation Active / Inserted (Pure Blue) */
    LED_COLOR_WHITE,    /* Activity Flash (POST / Read) */
} status_led_color_t;

esp_err_t status_led_init(void);
void status_led_set_color(status_led_color_t color);
void status_led_set_rgb(uint8_t r, uint8_t g, uint8_t b);
void status_led_activity_flash(status_led_color_t flash_color);

#endif /* STATUS_LED_H */
