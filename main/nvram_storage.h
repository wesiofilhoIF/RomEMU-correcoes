#ifndef NVRAM_STORAGE_H
#define NVRAM_STORAGE_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/*
 * NVRAM Storage Manager for UEFI BIOS Emulation:
 * 
 * In Intel x86 / AMI Aptio V UEFI architecture (16MB ROM):
 * - NVRAM Volume 1: 0x800000 - 0x840000 (256 KiB)
 * - NVRAM Volume 2: 0x840000 - 0x880000 (256 KiB - Mirror / Backup)
 * Total tracked window: 512 KiB (0x800000 to 0x880000).
 *
 * When the motherboard saves BIOS setup changes (boot order, fan curves,
 * overclocking, timestamps), the chipset issues 4KB Sector Erases and
 * 256-byte Page Programs inside this exact 512KB window.
 *
 * This module tracks modifications in PSRAM and persists them to the
 * ESP32-S3 internal Flash partition (bios_storage offset +5MB)
 * using an asynchronous debounced background task (saves 3s after last write).
 */

#define NVRAM_BASE_ADDR     0x00800000UL
#define NVRAM_SIZE          0x00080000UL  /* 512 KiB */
#define NVRAM_FLASH_OFFSET  0x00500000UL  /* Offset inside bios_storage partition (after 5MB compressed ROM) */

#define NVRAM_MAGIC         0x4E56524DU  /* 'NVRM' */

typedef struct {
    uint32_t magic;
    uint32_t size;
    uint32_t crc32;
    uint32_t write_count;
    uint32_t last_saved_timestamp;
} __attribute__((packed)) nvram_header_t;

/**
 * Initialize NVRAM storage system.
 */
esp_err_t nvram_storage_init(void);

/**
 * Notify NVRAM subsystem that a write/erase occurred in SPI flash.
 * If addr falls within 0x800000 - 0x880000, marks dirty and schedules auto-save.
 */
void nvram_storage_notify_write(uint32_t addr, uint32_t len);

/**
 * Restore persisted NVRAM from ESP32 Flash into the active cstore/slot in PSRAM.
 * Called automatically during boot after mounting the base BIOS image.
 */
esp_err_t nvram_storage_restore_to_active(void);

/**
 * Force immediate synchronous commit of NVRAM to internal Flash.
 */
esp_err_t nvram_storage_commit(void);

/**
 * Get NVRAM subsystem stats in JSON format.
 */
int nvram_storage_get_json(char *buf, size_t buf_size);

#endif /* NVRAM_STORAGE_H */
