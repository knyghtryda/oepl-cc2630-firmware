// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nathan Bigelow
#ifndef OEPL_OTA_CC2630_H
#define OEPL_OTA_CC2630_H

#include <stdint.h>
#include <stdbool.h>
#include "oepl_radio_cc2630.h"

// OTA flash layout (CC2630F128 = 128KB, 4KB sectors)
#define OTA_STAGING_ADDR    0x10000  // Sector 16
#define OTA_STAGING_END     0x1E000  // Sector 29 (exclusive)
#define OTA_STAGING_SIZE    (OTA_STAGING_END - OTA_STAGING_ADDR)  // 56KB
#define OTA_SECTOR_SIZE     0x1000   // 4KB

// Minimum supply for the irreversible staging→active copy. Erasing and
// programming 14 sectors draws the flash charge pump; a tag that browns out
// part-way through is bricked, and one that waits keeps its staged image and
// retries on a later check-in. Measured idle on fresh CR2450s: ~3000 mV;
// the display refresh already fails well before this.
#ifndef OTA_APPLY_MIN_MV
#define OTA_APPLY_MIN_MV    2400
#endif

// Sector 30 (0x1E000) stores the last applied OTA dataVer to prevent
// re-downloading the same firmware if the AP re-offers it.
#define OTA_DATAVER_ADDR    0x1E000
#define OTA_DATAVER_MAGIC   0x4F544156  // "OTAV"

// What the last staging→active copy did, stored alongside the dataVer record
// in sector 30. apply_ota() runs from RAM with interrupts off and cannot call
// the logger, so this is the only way to see inside it — and it has to survive
// into a *different* image, which rules out a RAM address (.noinit moves
// between builds). The `reported` byte starts out erased (0xFF) and the new
// image clears it to 0 once it has said so, which needs no sector erase.
struct ota_apply_stat {
    uint8_t sectors;     // sectors copied
    uint8_t retries;     // erase/program attempts beyond the first, total
    uint8_t bad;         // sectors that never read back correctly
};

// True once per apply, on the first boot of the image that was installed;
// clears the flag in flash so later boots stay quiet.
bool oepl_ota_take_apply_report(struct ota_apply_stat *out);

// Download firmware to staging flash, verify, copy to active area, reboot.
// Does NOT return on success. On failure, returns so caller can retry later.
void oepl_ota_download_and_apply(struct AvailDataInfo *info);

// Download info->dataSize bytes into flash at `base` (each block checksummed
// and read back with the cache invalidated). Shared by the firmware and
// compressed-image paths.
bool oepl_ota_stage_download(struct AvailDataInfo *info, uint32_t base,
                             uint32_t max_bytes);

// Flash primitives (ROM FSM, interrupts masked, standby re-enabled after).
uint32_t oepl_flash_erase_sector(uint32_t addr);
uint32_t oepl_flash_program(const uint8_t *data, uint32_t addr, uint32_t len);
void oepl_flash_cache_invalidate(void);

// Check if the offered dataVer matches the last successfully applied OTA.
// Returns true if the tag already has this firmware.
bool oepl_ota_already_applied(uint64_t dataVer);

#endif // OEPL_OTA_CC2630_H
