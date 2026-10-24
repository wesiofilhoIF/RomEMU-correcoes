#include "status_led.h"
#include "pin_config.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "status_led";

#if PIN_LED_STATUS != GPIO_NUM_NC

static rmt_channel_handle_t s_led_chan = NULL;
static rmt_encoder_handle_t s_led_encoder = NULL;
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

    /* Reset pulse: > 50us (500 ticks of 100ns = 50us) */
    ws2812_encoder->reset_code = (rmt_symbol_word_t){
        .level0 = 0, .duration0 = 300,
        .level1 = 0, .duration1 = 300
    };

    *ret_encoder = &ws2812_encoder->base;
    return ESP_OK;
}

esp_err_t status_led_init(void)
{
    ESP_LOGI(TAG, "Initializing Status RGB LED on GPIO %d", PIN_LED_STATUS);

    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = PIN_LED_STATUS,
        .mem_block_symbols = 64,
        .resolution_hz = 10000000, /* 10MHz */
        .trans_queue_depth = 4,
    };

    esp_err_t err = rmt_new_tx_channel(&tx_chan_config, &s_led_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RMT TX channel: %s", esp_err_to_name(err));
        return err;
    }

    err = rmt_new_ws2812_encoder(&s_led_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create WS2812 encoder: %s", esp_err_to_name(err));
        return err;
    }

    rmt_enable(s_led_chan);
    status_led_set_color(LED_COLOR_YELLOW); /* Booting yellow */
    return ESP_OK;
}

void status_led_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_led_chan || !s_led_encoder) return;

    /* WS2812 expects GRB byte order */
    uint8_t grb[3] = { g, r, b };
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    rmt_transmit(s_led_chan, s_led_encoder, grb, sizeof(grb), &tx_config);
}

void status_led_set_color(status_led_color_t color)
{
    s_base_color = color;
    /* Gentle brightness (max ~40/255) to avoid blinding on bench */
    switch (color) {
    case LED_COLOR_BLACK:   status_led_set_rgb(0, 0, 0); break;
    case LED_COLOR_RED:     status_led_set_rgb(35, 0, 0); break;
    case LED_COLOR_GREEN:   status_led_set_rgb(0, 35, 0); break;
    case LED_COLOR_BLUE:    status_led_set_rgb(0, 0, 35); break;
    case LED_COLOR_YELLOW:  status_led_set_rgb(30, 25, 0); break;
    case LED_COLOR_CYAN:    status_led_set_rgb(0, 25, 30); break;
    case LED_COLOR_MAGENTA: status_led_set_rgb(30, 0, 30); break;
    case LED_COLOR_WHITE:   status_led_set_rgb(25, 25, 25); break;
    case LED_COLOR_ORANGE:  status_led_set_rgb(35, 12, 0); break;
    default: break;
    }
}

void status_led_activity_flash(status_led_color_t flash_color)
{
    status_led_set_color(flash_color);
    vTaskDelay(pdMS_TO_TICKS(15));
    status_led_set_color(s_base_color);
}

#else

esp_err_t status_led_init(void) { return ESP_OK; }
void status_led_set_color(status_led_color_t color) {}
void status_led_set_rgb(uint8_t r, uint8_t g, uint8_t b) {}
void status_led_activity_flash(status_led_color_t flash_color) {}

#endif
