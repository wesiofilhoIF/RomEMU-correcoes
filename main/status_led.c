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
 * WS2812 RGB inteligente: GPIO 48 (v1.0) e GPIO 38 (v1.1)
 * Desativados LEDs comuns em GPIO 13 pois 13 é o MISO da BIOS!
 */
static const int s_rgb_pins[] = { 48, 38 };
#define NUM_RGB_PINS  (sizeof(s_rgb_pins) / sizeof(s_rgb_pins[0]))
#define NUM_GPIO_PINS 0

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

    /* 2. Standard GPIO indicator loop removed to avoid bus collisions */

    status_led_set_color(LED_COLOR_BLACK);
    return ESP_OK;
}

/* Gamma LUT (gamma = 2.2) for smooth perceptual brightness and accurate color blending */
static const uint8_t s_gamma_lut[256] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   1,   1,   1,   1,   1,   1,   1,   1,   1,   2,
    2,   2,   2,   2,   2,   3,   3,   3,   3,   4,   4,   4,   4,   5,   5,   5,
    5,   6,   6,   6,   7,   7,   7,   8,   8,   8,   9,   9,   9,  10,  10,  11,
   11,  11,  12,  12,  13,  13,  14,  14,  15,  15,  16,  16,  17,  17,  18,  18,
   19,  19,  20,  20,  21,  21,  22,  23,  23,  24,  24,  25,  26,  26,  27,  28,
   28,  29,  30,  30,  31,  32,  32,  33,  34,  35,  35,  36,  37,  38,  38,  39,
   40,  41,  42,  42,  43,  44,  45,  46,  47,  47,  48,  49,  50,  51,  52,  53,
   54,  55,  56,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,
   69,  70,  71,  73,  74,  75,  76,  77,  78,  79,  80,  81,  82,  84,  85,  86,
   87,  88,  90,  91,  92,  93,  95,  96,  97,  99, 100, 101, 102, 104, 105, 106,
  108, 109, 110, 112, 113, 115, 116, 117, 119, 120, 122, 123, 125, 126, 128, 129,
  131, 132, 134, 135, 137, 139, 140, 142, 143, 145, 147, 148, 150, 152, 153, 155,
  157, 158, 160, 162, 164, 165, 167, 169, 171, 173, 175, 176, 178, 180, 182, 184,
  186, 188, 190, 192, 194, 196, 198, 200, 202, 204, 206, 208, 210, 213, 215, 217,
  219, 221, 224, 226, 228, 230, 233, 235, 237, 240, 242, 244, 247, 249, 252, 255
};

void status_led_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    /* Apply CIE/FastLED White Balancing: Green eye sensitivity is ~2.5x higher than blue */
    /* Scaled for comfortable indoor bench brightness (max output ~65) */
    uint8_t r_corr = (uint8_t)(((uint32_t)r * 255) / 255);
    uint8_t g_corr = (uint8_t)(((uint32_t)g * 170) / 255);
    uint8_t b_corr = (uint8_t)(((uint32_t)b * 220) / 255);

    /* Apply non-linear Gamma 2.2 LUT */
    uint8_t grb[3] = {
        s_gamma_lut[g_corr],
        s_gamma_lut[r_corr],
        s_gamma_lut[b_corr]
    };
    rmt_transmit_config_t tx_config = { .loop_count = 0 };

    /* Update all RGB outputs simultaneously */
    for (size_t i = 0; i < NUM_RGB_PINS; i++) {
        if (s_led_chans[i] && s_led_encoders[i]) {
            rmt_transmit(s_led_chans[i], s_led_encoders[i], grb, sizeof(grb), &tx_config);
        }
    }

    /* Update standard digital LEDs: ON if any color component is active, OFF if black */
    /* Standard GPIO indicators removed to keep GPIO 13 clean for MISO */
}

/* Transições claras e sem confusão de cores:
   - LED_COLOR_BLACK: Desligado
   - LED_COLOR_RED: Erro crítico
   - LED_COLOR_CYAN: Pronto / Emulação ativa em repouso (Ciano/Turquesa suave)
   - LED_COLOR_BLUE: CS=1 (Azul 100% puro piscando no loop SPI)
   - LED_COLOR_GREEN: Leitura / Transação SPI ativa em andamento (Verde vibrante único!)
   - LED_COLOR_MAGENTA: Upload de BIOS
   - LED_COLOR_YELLOW: Inicialização / Boot
*/
void status_led_set_color(status_led_color_t color)
{
    s_base_color = color;
    switch (color) {
    case LED_COLOR_BLACK:   status_led_set_rgb(0, 0, 0); break;
    case LED_COLOR_RED:     status_led_set_rgb(255, 0, 0); break;    /* Solid Red */
    case LED_COLOR_GREEN:   status_led_set_rgb(0, 255, 0); break;    /* Verde brilhante 100% puro e único */
    case LED_COLOR_BLUE:    status_led_set_rgb(0, 0, 255); break;    /* Solid Blue */
    case LED_COLOR_YELLOW:  status_led_set_rgb(220, 90, 0); break;   /* Warm Amber */
    case LED_COLOR_CYAN:    status_led_set_rgb(0, 60, 180); break;   /* Ciano suave para Standby */
    case LED_COLOR_MAGENTA: status_led_set_rgb(180, 0, 220); break;  /* Magenta / Purple */
    case LED_COLOR_WHITE:   status_led_set_rgb(160, 140, 160); break;/* Neutral White */
    case LED_COLOR_ORANGE:  status_led_set_rgb(220, 50, 0); break;   /* Vivid Orange */
    }
}

void status_led_activity_flash(status_led_color_t flash_color)
{
    status_led_set_color(flash_color);
    vTaskDelay(pdMS_TO_TICKS(10));
    status_led_set_color(s_base_color);
}

void status_led_standby_breathe_step(void)
{
    static int s_brightness = 10;
    static int s_delta = 8;

    s_brightness += s_delta;
    if (s_brightness >= 240) {
        s_brightness = 240;
        s_delta = -8;
    } else if (s_brightness <= 10) {
        s_brightness = 10;
        s_delta = 8;
    }

    /* Amarelo puro vibrante: R e G com a mesma intensidade de brilho modulada pelo fade */
    status_led_set_rgb((uint8_t)s_brightness, (uint8_t)s_brightness, 0);
}

/* Converte matiz HSV (0 a 359) para RGB puro */
static void hsv2rgb(uint16_t h, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t region = h / 60;
    uint8_t remainder = (h - (region * 60)) * 255 / 60;
    uint8_t q = 255 - remainder;
    uint8_t t = remainder;

    switch (region) {
        case 0:  *r = 255; *g = t;   *b = 0;   break;
        case 1:  *r = q;   *g = 255; *b = 0;   break;
        case 2:  *r = 0;   *g = 255; *b = t;   break;
        case 3:  *r = 0;   *g = q;   *b = 255; break;
        case 4:  *r = t;   *g = 0;   *b = 255; break;
        default: *r = 255; *g = 0;   *b = q;   break;
    }
}

void status_led_celebrate_rainbow(uint32_t duration_ms)
{
    uint32_t steps = duration_ms / 15;
    if (steps == 0) steps = 1;

    /* Ciclo Rainbow comemorativo rápido e vibrante (múltiplas voltas) */
    for (uint32_t i = 0; i < steps; i++) {
        uint16_t hue = (i * 720 / steps) % 360; /* 2 ciclos completos de arco-íris */
        uint8_t r = 0, g = 0, b = 0;
        hsv2rgb(hue, &r, &g, &b);
        status_led_set_rgb(r, g, b);
        vTaskDelay(pdMS_TO_TICKS(15));
    }

    /* Termina e crava no Verde de sucesso */
    status_led_set_color(LED_COLOR_GREEN);
}

