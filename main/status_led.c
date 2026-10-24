#include "status_led.h"
#include "pin_config.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "status_led";

/*
 * ============================================================================
 * Matriz Fixa de Pinos para Produção (ESP32-S3 DevKitC + UNO D1 + SuperMini)
 * ============================================================================
 * - WS2812 RGB inteligente: GPIO 48 (v1.0) e GPIO 38 (v1.1)
 * - LEDs simples comuns (HIGH/LOW): GPIO 2 (D9), GPIO 13 (D13 UNO), GPIO 21
 */
static const int s_rgb_pins[] = { 48, 38 };
static const int s_gpio_pins[] = { 2, 13, 21 };

#define NUM_RGB_PINS  (sizeof(s_rgb_pins) / sizeof(s_rgb_pins[0]))
#define NUM_GPIO_PINS (sizeof(s_gpio_pins) / sizeof(s_gpio_pins[0]))

static rmt_channel_handle_t s_led_chans[NUM_RGB_PINS] = { NULL };
static rmt_encoder_handle_t s_led_encoders[NUM_RGB_PINS] = { NULL };
static status_led_color_t s_base_color = LED_COLOR_BLACK;

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
} rmt_ws2812_encoder_t;

static size_t rmt_encode_ws2812(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                                const void *primary_data, size_t data_size,
                                rmt_encode_state_t *ret_state)
{
    rmt_ws2812_encoder_t *ws2812_encoder = __containerof(encoder, rmt_ws2812_encoder_t, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;

    switch (ws2812_encoder->state) {
    case 0:
        encoded_symbols += ws2812_encoder->bytes_encoder->encode(
            ws2812_encoder->bytes_encoder, channel, primary_data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            ws2812_encoder->state = 1;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto out;
        }
        /* fall-through */
    case 1:
        encoded_symbols += ws2812_encoder->copy_encoder->encode(
            ws2812_encoder->copy_encoder, channel, &ws2812_encoder->reset_code,
            sizeof(ws2812_encoder->reset_code), &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            ws2812_encoder->state = 0;
            state |= RMT_ENCODING_COMPLETE;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto out;
        }
    }
out:
    *ret_state = state;
    return encoded_symbols;
}

static esp_err_t rmt_del_ws2812_encoder(rmt_encoder_t *encoder)
{
    rmt_ws2812_encoder_t *ws2812_encoder = __containerof(encoder, rmt_ws2812_encoder_t, base);
    rmt_del_encoder(ws2812_encoder->bytes_encoder);
    rmt_del_encoder(ws2812_encoder->copy_encoder);
    free(ws2812_encoder);
    return ESP_OK;
}

static esp_err_t rmt_reset_ws2812_encoder(rmt_encoder_t *encoder)
{
    rmt_ws2812_encoder_t *ws2812_encoder = __containerof(encoder, rmt_ws2812_encoder_t, base);
    rmt_encoder_reset(ws2812_encoder->bytes_encoder);
    rmt_encoder_reset(ws2812_encoder->copy_encoder);
    ws2812_encoder->state = 0;
    return ESP_OK;
}

static esp_err_t rmt_new_ws2812_encoder(rmt_encoder_handle_t *ret_encoder)
{
    rmt_ws2812_encoder_t *ws2812_encoder = calloc(1, sizeof(rmt_ws2812_encoder_t));
    if (!ws2812_encoder) return ESP_ERR_NO_MEM;

    ws2812_encoder->base.encode = rmt_encode_ws2812;
    ws2812_encoder->base.del = rmt_del_ws2812_encoder;
    ws2812_encoder->base.reset = rmt_reset_ws2812_encoder;

    /* 10MHz resolution: 1 tick = 100ns */
    rmt_bytes_encoder_config_t bytes_encoder_config = {
        .bit0 = {
            .level0 = 1, .duration0 = 3,  /* 300ns HIGH */
            .level1 = 0, .duration1 = 9   /* 900ns LOW */
        },
        .bit1 = {
            .level0 = 1, .duration0 = 9,  /* 900ns HIGH */
            .level1 = 0, .duration1 = 3   /* 300ns LOW */
        },
        .flags.msb_first = 1
    };
    rmt_new_bytes_encoder(&bytes_encoder_config, &ws2812_encoder->bytes_encoder);

    rmt_copy_encoder_config_t copy_encoder_config = {};
    rmt_new_copy_encoder(&copy_encoder_config, &ws2812_encoder->copy_encoder);

    /* Reset pulse: > 50us */
    ws2812_encoder->reset_code = (rmt_symbol_word_t){
        .level0 = 0, .duration0 = 300,
        .level1 = 0, .duration1 = 300
    };

    *ret_encoder = &ws2812_encoder->base;
    return ESP_OK;
}

esp_err_t status_led_init(void)
{
    ESP_LOGI(TAG, "Initializing Universal Multi-Pin Status LEDs for Production");

    /* 1. Init RGB WS2812 RMT channels on candidate pins (GPIO 48 & GPIO 38) */
    for (size_t i = 0; i < NUM_RGB_PINS; i++) {
        rmt_tx_channel_config_t tx_chan_config = {
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .gpio_num = (gpio_num_t)s_rgb_pins[i],
            .mem_block_symbols = 64,
            .resolution_hz = 10000000,
            .trans_queue_depth = 4,
        };
        if (rmt_new_tx_channel(&tx_chan_config, &s_led_chans[i]) == ESP_OK) {
            if (rmt_new_ws2812_encoder(&s_led_encoders[i]) == ESP_OK) {
                rmt_enable(s_led_chans[i]);
            }
        }
    }

    /* 2. Init Standard GPIO indicator pins (GPIO 2, 13, 21) as outputs */
    for (size_t i = 0; i < NUM_GPIO_PINS; i++) {
        gpio_reset_pin((gpio_num_t)s_gpio_pins[i]);
        gpio_set_direction((gpio_num_t)s_gpio_pins[i], GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)s_gpio_pins[i], 0);
    }

    status_led_set_color(LED_COLOR_YELLOW);
    return ESP_OK;
}

void status_led_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t grb[3] = { g, r, b };
    rmt_transmit_config_t tx_config = { .loop_count = 0 };

    /* Update all RGB outputs simultaneously */
    for (size_t i = 0; i < NUM_RGB_PINS; i++) {
        if (s_led_chans[i] && s_led_encoders[i]) {
            rmt_transmit(s_led_chans[i], s_led_encoders[i], grb, sizeof(grb), &tx_config);
        }
    }

    /* Update standard digital LEDs: ON if any color component is active, OFF if black */
    int level = (r > 0 || g > 0 || b > 0) ? 1 : 0;
    for (size_t i = 0; i < NUM_GPIO_PINS; i++) {
        gpio_set_level((gpio_num_t)s_gpio_pins[i], level);
    }
}

void status_led_set_color(status_led_color_t color)
{
    s_base_color = color;
    switch (color) {
    case LED_COLOR_BLACK:   status_led_set_rgb(0, 0, 0); break;
    case LED_COLOR_RED:     status_led_set_rgb(45, 0, 0); break;
    case LED_COLOR_GREEN:   status_led_set_rgb(0, 45, 0); break;
    case LED_COLOR_BLUE:    status_led_set_rgb(0, 0, 45); break;
    case LED_COLOR_YELLOW:  status_led_set_rgb(40, 35, 0); break;
    case LED_COLOR_CYAN:    status_led_set_rgb(0, 35, 40); break;
    case LED_COLOR_MAGENTA: status_led_set_rgb(40, 0, 40); break;
    case LED_COLOR_WHITE:   status_led_set_rgb(35, 35, 35); break;
    case LED_COLOR_ORANGE:  status_led_set_rgb(45, 18, 0); break;
    default: break;
    }
}

void status_led_activity_flash(status_led_color_t flash_color)
{
    status_led_set_color(flash_color);
    vTaskDelay(pdMS_TO_TICKS(15));
    status_led_set_color(s_base_color);
}

