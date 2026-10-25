#include "i2c_eeprom_emu.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "i2c_emu";

emu_stats_t g_i2c_stats;

esp_err_t i2c_eeprom_emu_init(void)
{
    memset(&g_i2c_stats, 0, sizeof(g_i2c_stats));
    ESP_LOGI(TAG, "I2C EEPROM emulator stub initialized (SPI Flash mode active)");
    return ESP_OK;
}

esp_err_t i2c_eeprom_emu_start(chip_type_t chip)
{
    (void)chip;
    return ESP_OK;
}

esp_err_t i2c_eeprom_emu_stop(void)
{
    return ESP_OK;
}

esp_err_t i2c_eeprom_emu_set_chip(chip_type_t chip)
{
    (void)chip;
    return ESP_OK;
}

const emu_stats_t *i2c_eeprom_emu_get_stats(void)
{
    return &g_i2c_stats;
}

void i2c_eeprom_emu_reset_stats(void)
{
    memset(&g_i2c_stats, 0, sizeof(g_i2c_stats));
}
