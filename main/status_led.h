#ifndef STATUS_LED_H
#define STATUS_LED_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    LED_COLOR_BLACK = 0,
    LED_COLOR_RED,      /* Error / Fault */
    LED_COLOR_GREEN,    /* Ready / Idle */
    LED_COLOR_BLUE,     /* WiFi connecting */
    LED_COLOR_YELLOW,   /* Booting */
    LED_COLOR_CYAN,     /* Emulation Active (Inserted) */
    LED_COLOR_MAGENTA,  /* Upload in progress */
    LED_COLOR_WHITE,    /* SPI Read activity (Flash POST) */
    LED_COLOR_ORANGE    /* SPI Write activity */
} status_led_color_t;

esp_err_t status_led_init(void);
void status_led_set_color(status_led_color_t color);
void status_led_set_rgb(uint8_t r, uint8_t g, uint8_t b);
void status_led_activity_flash(status_led_color_t flash_color);

#endif /* STATUS_LED_H */
