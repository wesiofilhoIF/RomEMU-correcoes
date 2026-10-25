#include "nvram_storage.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "rom_store.h"
#include "compressed_store.h"

static const char *TAG = "nvram_store";

static const esp_partition_t *s_part = NULL;
static bool s_dirty = false;
static uint32_t s_last_write_tick = 0;
static uint32_t s_write_count = 0;
static uint32_t s_save_count = 0;
static TaskHandle_t s_sync_task = NULL;
static SemaphoreHandle_t s_mutex = NULL;

static void nvram_sync_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "NVRAM background sync task running (debounced auto-save enabled)");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!s_dirty) continue;

        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        /* If 3 seconds passed since last write, flush NVRAM to Flash */
        if (now - s_last_write_tick >= 3000) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            if (s_dirty) {
                ESP_LOGI(TAG, "Flushing modified BIOS NVRAM to internal Flash...");
                esp_err_t err = nvram_storage_commit();
                if (err == ESP_OK) {
                    s_dirty = false;
                    s_save_count++;
                    ESP_LOGI(TAG, "BIOS NVRAM saved successfully! Total saves: %lu", (unsigned long)s_save_count);
                } else {
                    ESP_LOGE(TAG, "Failed to save NVRAM: %s", esp_err_to_name(err));
                }
            }
            xSemaphoreGive(s_mutex);
        }
    }
}

esp_err_t nvram_storage_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x99, "bios_storage");
    if (!s_part) {
        ESP_LOGW(TAG, "bios_storage partition not found! NVRAM persistence disabled.");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "NVRAM persistence initialized. Storage partition: %s (offset 0x%lx, size %lu KB)",
             s_part->label, (unsigned long)(s_part->address + NVRAM_FLASH_OFFSET),
             (unsigned long)(NVRAM_SIZE / 1024));

    xTaskCreate(nvram_sync_task, "nvram_sync", 4096, NULL, 5, &s_sync_task);
    return ESP_OK;
}

void nvram_storage_notify_write(uint32_t addr, uint32_t len)
{
    /* Check if written block intersects NVRAM window (0x800000 - 0x880000) */
    uint32_t end_addr = addr + len;
    if (end_addr > NVRAM_BASE_ADDR && addr < (NVRAM_BASE_ADDR + NVRAM_SIZE)) {
        s_dirty = true;
        s_write_count++;
        s_last_write_tick = (uint32_t)(esp_timer_get_time() / 1000);
    }
}

esp_err_t nvram_storage_commit(void)
{
    if (!s_part) return ESP_ERR_NOT_FOUND;

    rom_slot_t *slot = rom_store_get_active(BUS_SPI);
    if (!slot || (!slot->data && !slot->cstore)) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Allocate buffer for 512KB NVRAM */
    uint8_t *nvram_buf = heap_caps_malloc(NVRAM_SIZE, MALLOC_CAP_SPIRAM);
    if (!nvram_buf) nvram_buf = heap_caps_malloc(NVRAM_SIZE, MALLOC_CAP_INTERNAL);
    if (!nvram_buf) return ESP_ERR_NO_MEM;

    /* Read current live NVRAM data from emulated memory */
    if (slot->compressed && slot->cstore) {
        cstore_read(slot->cstore, NVRAM_BASE_ADDR, nvram_buf, NVRAM_SIZE);
    } else if (slot->data) {
        memcpy(nvram_buf, &slot->data[NVRAM_BASE_ADDR], NVRAM_SIZE);
    }

    uint32_t crc = rom_store_crc32(nvram_buf, NVRAM_SIZE);

    nvram_header_t hdr = {
        .magic = NVRAM_MAGIC,
        .size = NVRAM_SIZE,
        .crc32 = crc,
        .write_count = s_write_count,
        .last_saved_timestamp = (uint32_t)(esp_timer_get_time() / 1000000)
    };

    /* Erase dedicated 512KB + 4KB sector in Flash */
    esp_err_t err = esp_partition_erase_range(s_part, NVRAM_FLASH_OFFSET, NVRAM_SIZE + 4096);
    if (err != ESP_OK) {
        heap_caps_free(nvram_buf);
        return err;
    }

    /* Write header */
    err = esp_partition_write(s_part, NVRAM_FLASH_OFFSET, &hdr, sizeof(hdr));
    if (err == ESP_OK) {
        /* Write 512KB payload */
        err = esp_partition_write(s_part, NVRAM_FLASH_OFFSET + 4096, nvram_buf, NVRAM_SIZE);
    }

    heap_caps_free(nvram_buf);
    return err;
}

esp_err_t nvram_storage_restore_to_active(void)
{
    if (!s_part) return ESP_ERR_NOT_FOUND;

    nvram_header_t hdr;
    esp_err_t err = esp_partition_read(s_part, NVRAM_FLASH_OFFSET, &hdr, sizeof(hdr));
    if (err != ESP_OK || hdr.magic != NVRAM_MAGIC || hdr.size != NVRAM_SIZE) {
        ESP_LOGI(TAG, "No previously saved NVRAM found in Flash (first boot stock setup)");
        return ESP_OK;
    }

    rom_slot_t *slot = rom_store_get_active(BUS_SPI);
    if (!slot || (!slot->data && !slot->cstore)) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *nvram_buf = heap_caps_malloc(NVRAM_SIZE, MALLOC_CAP_SPIRAM);
    if (!nvram_buf) nvram_buf = heap_caps_malloc(NVRAM_SIZE, MALLOC_CAP_INTERNAL);
    if (!nvram_buf) return ESP_ERR_NO_MEM;

    err = esp_partition_read(s_part, NVRAM_FLASH_OFFSET + 4096, nvram_buf, NVRAM_SIZE);
    if (err == ESP_OK) {
        uint32_t crc = rom_store_crc32(nvram_buf, NVRAM_SIZE);
        if (crc == hdr.crc32) {
            ESP_LOGI(TAG, "Restoring %lu KB saved BIOS setup & NVRAM variables into PSRAM (CRC: 0x%08lx)...",
                     (unsigned long)(NVRAM_SIZE / 1024), (unsigned long)crc);
            if (slot->compressed && slot->cstore) {
                cstore_write(slot->cstore, NVRAM_BASE_ADDR, nvram_buf, NVRAM_SIZE, false);
            } else if (slot->data) {
                memcpy(&slot->data[NVRAM_BASE_ADDR], nvram_buf, NVRAM_SIZE);
            }
            ESP_LOGI(TAG, ">>> SUCCESS: BIOS Setup settings restored! Boots with your exact overclock/config! <<<");
        } else {
            ESP_LOGW(TAG, "NVRAM Flash CRC mismatch (0x%08lx != 0x%08lx), skipping restore",
                     (unsigned long)crc, (unsigned long)hdr.crc32);
        }
    }

    heap_caps_free(nvram_buf);
    return err;
}

int nvram_storage_get_json(char *buf, size_t buf_size)
{
    return snprintf(buf, buf_size,
        "{\"enabled\":%s,\"dirty\":%s,\"writes\":%lu,\"saves\":%lu,\"base\":\"0x%08lX\",\"size\":%lu}",
        s_part ? "true" : "false",
        s_dirty ? "true" : "false",
        (unsigned long)s_write_count,
        (unsigned long)s_save_count,
        (unsigned long)NVRAM_BASE_ADDR,
        (unsigned long)NVRAM_SIZE);
}
