// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nathan Bigelow
// -----------------------------------------------------------------------------
//  OTA Firmware Update for CC2630 OEPL Tag
//
//  Downloads firmware blocks to staging flash (sectors 16-30), then copies
//  staging to active area (sectors 0-15) using a RAM-resident function.
//
//  Flash layout (CC2630F128 = 128KB):
//    0x00000 - 0x0FFFF:  Active firmware (64KB max, sectors 0-15)
//    0x10000 - 0x1DFFF:  OTA staging area (56KB, sectors 16-29)
//    0x1E000 - 0x1EFFF:  OTA dataVer tracking (sector 30)
//    0x1F000 - 0x1FFFF:  CCFG sector (never touched)
// -----------------------------------------------------------------------------

#include "oepl_ota_cc2630.h"
#include "oepl_radio_cc2630.h"
#include "oepl_hw_abstraction_cc2630.h"
#include "oepl_rf_cc2630.h"
#include "rtt.h"
#include "cpu.h"
#include "vims.h"
#include "hw_memmap.h"
#include "hw_types.h"
#include "hw_flash.h"
#include <string.h>

// ROM API for flash operations (from rom.h)
// ROM_API_TABLE is at 0x10000180
// ROM_API_FLASH_TABLE = ROM_API_TABLE[10]
// FlashSectorErase = ROM_API_FLASH_TABLE[5]
// FlashProgram     = ROM_API_FLASH_TABLE[6]
// (driverlib's rom.h defines these identically when it is in scope)
#ifndef ROM_API_TABLE
#define ROM_API_TABLE       ((uint32_t *)0x10000180)
#endif
#ifndef ROM_API_FLASH_TABLE
#define ROM_API_FLASH_TABLE ((uint32_t *)(ROM_API_TABLE[10]))
#endif

// Flash status codes
#define FAPI_STATUS_SUCCESS 0x00000000

// ROM Hard-API table (HAPI) for ResetDevice
// HAPI table at 0x10000048, ResetDevice is at offset 6 (7th entry)
#define ROM_HAPI_TABLE_ADDR 0x10000048

// Flash function pointer types
typedef uint32_t (*flash_erase_fn_t)(uint32_t addr);
typedef uint32_t (*flash_program_fn_t)(uint8_t *buf, uint32_t addr, uint32_t len);

// --- Flash helpers for staging area writes ---
// These run from flash. The rule is not "don't touch the sector you run from":
// no code may execute from flash while ANY part of the bank is in an erase or
// program cycle. That holds here only because the ROM FSM call itself stalls
// the CPU and nothing arms an NVIC source during OTA — so mask interrupts
// around the call, which costs nothing against a multi-millisecond flash
// operation and keeps this true if an interrupt source is ever added.

static void flash_call_done(void)
{
    // driverlib's FlashSectorErase()/FlashProgram() clear this after every
    // operation; calling the ROM entry directly skips that and can leave the
    // flash bank unable to enter standby.
    HWREGBITW(FLASH_BASE + FLASH_O_CFG, FLASH_CFG_DIS_STANDBY_BITN) = 0;
}

static uint32_t staging_erase_sector(uint32_t addr)
{
    flash_erase_fn_t erase = (flash_erase_fn_t)ROM_API_FLASH_TABLE[5];
    uint32_t primask = CPUcpsid();
    uint32_t rc = erase(addr);
    flash_call_done();
    if (!primask) CPUcpsie();
    return rc;
}

static uint32_t staging_program(uint8_t *data, uint32_t addr, uint32_t len)
{
    flash_program_fn_t program = (flash_program_fn_t)ROM_API_FLASH_TABLE[6];
    uint32_t primask = CPUcpsid();
    uint32_t rc = program(data, addr, len);
    flash_call_done();
    if (!primask) CPUcpsie();
    return rc;
}

// The VIMS cache does not snoop flash writes, so a readback right after a
// program can be served from lines populated before the erase. Bouncing the
// mode through OFF invalidates the cache; everything read afterwards comes
// from the flash array.
static void vims_invalidate(void)
{
    uint32_t mode = VIMSModeGet(VIMS_BASE);
    if (mode == VIMS_MODE_OFF) return;
    VIMSModeSafeSet(VIMS_BASE, VIMS_MODE_OFF, true);
    VIMSModeSafeSet(VIMS_BASE, mode, true);
}

// Verify flash contents match RAM buffer (call vims_invalidate() first)
static bool staging_verify(uint32_t addr, const uint8_t *data, uint32_t len)
{
    const uint8_t *flash = (const uint8_t *)addr;
    for (uint32_t i = 0; i < len; i++) {
        if (flash[i] != data[i]) return false;
    }
    return true;
}

// --- DataVer tracking (prevents OTA loops) ---
// Stored in sector 30 (0x1E000) which is outside the staging area.

struct __attribute__((packed)) OtaDataVerRecord {
    uint32_t magic;     // OTA_DATAVER_MAGIC
    uint64_t dataVer;
    uint8_t  sectors;   // what the apply that installed this image did
    uint8_t  retries;
    uint8_t  bad;
    uint8_t  reported;  // 0xFF until the installed image has reported the above
};
#define OTA_DATAVER_REC_LEN 16
#define OTA_REPORTED_OFFSET 15

bool oepl_ota_already_applied(uint64_t dataVer)
{
    const struct OtaDataVerRecord *rec =
        (const struct OtaDataVerRecord *)OTA_DATAVER_ADDR;
    return (rec->magic == OTA_DATAVER_MAGIC && rec->dataVer == dataVer);
}

// --- RAM-resident apply function ---
// This function runs entirely from SRAM. It erases active firmware sectors
// and programs them from staging, then triggers a system reset.
// It must NOT call any flash-resident functions, use string literals,
// or access any flash-based data. Pointer reads and writes go through
// volatile so the compiler can't turn a copy loop into a memcpy() call.
//
// It reuses bw_buf (4100 bytes in .bss / SRAM) as a copy buffer.
//
// There is no return path: the caller's code is erased by the first
// rom_erase(), so every failure here has to be handled in place. Each sector
// is erased, programmed and read back, and the whole erase/program/verify is
// retried before moving on — ram_buf still holds the data, so a retry is free.
// The VIMS cache must already be off (the caller turns it off), or the
// readback compares against cached bytes of the image being replaced.
//
// A sector that never verifies leaves a corrupt image either way, so the only
// thing left to do is withhold the "applied" record and reset.

#define APPLY_SECTOR_RETRIES 5

bool oepl_ota_take_apply_report(struct ota_apply_stat *out)
{
    const struct OtaDataVerRecord *rec =
        (const struct OtaDataVerRecord *)OTA_DATAVER_ADDR;
    if (rec->magic != OTA_DATAVER_MAGIC || rec->reported != 0xFF) return false;
    if (rec->sectors == 0xFF) {
        // Installed by firmware older than the stats (12-byte record): nothing
        // to say. Clear the flag so this is asked once.
        uint8_t zero0 = 0;
        staging_program(&zero0, OTA_DATAVER_ADDR + OTA_REPORTED_OFFSET, 1);
        return false;
    }
    out->sectors = rec->sectors;
    out->retries = rec->retries;
    out->bad = rec->bad;
    // Clear the flag: programming 0x00 over 0xFF only clears bits, so the rest
    // of the record survives without an erase.
    uint8_t zero = 0;
    staging_program(&zero, OTA_DATAVER_ADDR + OTA_REPORTED_OFFSET, 1);
    return true;
}

__attribute__((section(".ramfunc"), noinline, long_call))
static void apply_ota(uint32_t staging_addr, uint32_t fw_size, uint8_t *ram_buf,
                      uint64_t dataVer)
{
    // Disable all interrupts — we're about to erase our code
    __asm volatile("cpsid i");

    // Resolve ROM flash API function pointers
    // ROM_API_TABLE is in ROM (0x10000180), not flash — safe to read
    uint32_t *rom_table = (uint32_t *)0x10000180;
    uint32_t *flash_table = (uint32_t *)(rom_table[10]);
    flash_erase_fn_t rom_erase = (flash_erase_fn_t)flash_table[5];
    flash_program_fn_t rom_program = (flash_program_fn_t)flash_table[6];

    // Calculate number of active sectors to overwrite
    uint32_t num_sectors = (fw_size + OTA_SECTOR_SIZE - 1) / OTA_SECTOR_SIZE;
    uint32_t bad_sectors = 0;
    uint32_t retries = 0;

    // Copy each sector: staging → ram_buf → active
    for (uint32_t s = 0; s < num_sectors; s++) {
        uint32_t src_addr = staging_addr + s * OTA_SECTOR_SIZE;
        uint32_t dst_addr = s * OTA_SECTOR_SIZE;
        uint32_t chunk = fw_size - s * OTA_SECTOR_SIZE;
        if (chunk > OTA_SECTOR_SIZE) chunk = OTA_SECTOR_SIZE;

        // 1. Copy staging sector to RAM buffer
        //    (staging flash is still readable because we only erase active sectors)
        const volatile uint8_t *src = (const volatile uint8_t *)src_addr;
        volatile uint8_t *dst = (volatile uint8_t *)ram_buf;
        for (uint32_t i = 0; i < chunk; i++) {
            dst[i] = src[i];
        }
        // Pad remainder with 0xFF (erased state)
        for (uint32_t i = chunk; i < OTA_SECTOR_SIZE; i++) {
            dst[i] = 0xFF;
        }

        // 2. Erase, program and read back, retrying the whole sector on any
        //    FSM error or mismatch.
        bool ok = false;
        for (uint32_t attempt = 0; attempt < APPLY_SECTOR_RETRIES && !ok; attempt++) {
            if (attempt > 0) retries++;
            if (rom_erase(dst_addr) != FAPI_STATUS_SUCCESS) continue;
#ifdef BENCH_OTA_FLAKY_APPLY
            // Bench only: make the first attempt at sector 0 leave the sector
            // erased, so the readback fails and the retry path runs for real.
            if (s == 0 && attempt == 0) continue;
#endif
            if (rom_program(ram_buf, dst_addr, OTA_SECTOR_SIZE) != FAPI_STATUS_SUCCESS)
                continue;
            const volatile uint8_t *chk = (const volatile uint8_t *)dst_addr;
            ok = true;
            for (uint32_t i = 0; i < OTA_SECTOR_SIZE; i++) {
                if (chk[i] != dst[i]) { ok = false; break; }
            }
        }
        if (!ok) bad_sectors++;
    }

    // 3. Only a fully verified image gets the "this dataVer is installed"
    //    record. Written here, after the copy, so an interrupted apply is
    //    retried rather than skipped forever. A failed write is benign: the
    //    tag re-downloads the same firmware once. The same record carries what
    //    this copy did, for the installed image to report on its first boot
    //    (struct OtaDataVerRecord; `reported` starts erased).
    if (bad_sectors == 0) {
        volatile uint8_t *rec = (volatile uint8_t *)ram_buf;
        uint32_t magic = OTA_DATAVER_MAGIC;
        for (uint32_t i = 0; i < 4; i++) rec[i] = (uint8_t)(magic >> (8 * i));
        for (uint32_t i = 0; i < 8; i++) rec[4 + i] = (uint8_t)(dataVer >> (8 * i));
        rec[12] = (uint8_t)num_sectors;
        rec[13] = (uint8_t)(retries > 255 ? 255 : retries);
        rec[14] = (uint8_t)bad_sectors;
        rec[15] = 0xFF;                 // not reported yet
        if (rom_erase(OTA_DATAVER_ADDR) == FAPI_STATUS_SUCCESS) {
            rom_program(ram_buf, OTA_DATAVER_ADDR, OTA_DATAVER_REC_LEN);
        }
    }

    // Trigger system reset via ROM Hard-API ResetDevice function
    // HAPI table at 0x10000048, ResetDevice is entry 6 (offset 24 bytes)
    typedef void (*reset_fn_t)(void);
    uint32_t *hapi_table = (uint32_t *)0x10000048;
    reset_fn_t rom_reset = (reset_fn_t)hapi_table[6];
    rom_reset();

    // Should not reach here
    while (1) { __asm volatile("nop"); }
}

// --- Strict block download for OTA ---
// Requires ALL parts (no partial acceptance like image downloads).
// Retries up to 20 times. Returns true only if all parts received.
static bool ota_download_block(uint8_t block_id, struct AvailDataInfo *info,
                               uint8_t *buf, uint16_t *out_size)
{
    rtt_puts("B");
    rtt_put_hex8(block_id);

    uint8_t parts_rcvd[BLOCK_REQ_PARTS_BYTES];
    memset(parts_rcvd, 0, sizeof(parts_rcvd));
    memset(buf, 0xFF, BLOCK_XFER_BUFFER_SIZE);  // 0xFF = erased state (not 0x00)

    for (uint8_t attempt = 0; attempt < 20; attempt++) {
        if (attempt > 0) {
            rtt_puts("R");
            oepl_hw_delay_ms(500);
        }
        uint8_t got = oepl_radio_request_block(block_id, info->dataVer, info->dataType,
                                                buf, parts_rcvd);
        if (got >= BLOCK_MAX_PARTS) {
            rtt_puts("+");
            *out_size = BLOCK_XFER_BUFFER_SIZE;
            return true;
        }
        // OTA: NO partial acceptance — require ALL parts
    }
    rtt_puts("!");
    return false;
}

// Verify BlockData checksum: simple sum of all data bytes
static bool verify_block_checksum(const uint8_t *buf, uint32_t data_len)
{
    const struct BlockData *bd = (const struct BlockData *)buf;
    uint16_t expected = bd->checksum;
    uint16_t actual = 0;
    for (uint32_t i = 0; i < data_len; i++) {
        actual += buf[BLOCK_HEADER_SIZE + i];
    }
    return (actual == expected);
}

// Download `info->dataSize` bytes into flash at `base`, one 4 KB sector per
// block, checking each block's checksum and reading every sector back with the
// cache invalidated. Shared by the firmware path and the compressed-image
// path: both are far too big to hold in RAM. Logs the reason and returns
// false on any failure.
bool oepl_ota_stage_download(struct AvailDataInfo *info, uint32_t base,
                             uint32_t max_bytes)
{
    uint32_t size = info->dataSize;
    if (size == 0 || size > max_bytes) {
        rtt_puts("\r\nDL: size ");
        rtt_put_hex32(size);
        rtt_puts(" does not fit staging\r\n");
        return false;
    }

    uint32_t num_blocks = (size + BLOCK_DATA_SIZE - 1) / BLOCK_DATA_SIZE;
    rtt_puts("DL: blocks=");
    rtt_put_hex8((uint8_t)num_blocks);
    rtt_puts("\r\n");

    for (uint32_t block_id = 0; block_id < num_blocks; block_id++) {
        uint16_t block_size;

        // Strict: every part of every block, no partial acceptance
        if (!ota_download_block((uint8_t)block_id, info, bw_buf, &block_size)) {
            rtt_puts("\r\nDL: fail b=");
            rtt_put_hex8((uint8_t)block_id);
            rtt_puts("\r\n");
            return false;
        }

        uint32_t remaining = size - block_id * BLOCK_DATA_SIZE;
        uint32_t data_len = (remaining > BLOCK_DATA_SIZE) ? BLOCK_DATA_SIZE : remaining;

        if (!verify_block_checksum(bw_buf, data_len)) {
            rtt_puts("\r\nDL: checksum fail b=");
            rtt_put_hex8((uint8_t)block_id);
            rtt_puts("\r\n");
            return false;
        }
        rtt_puts("C");

        uint32_t addr = base + block_id * OTA_SECTOR_SIZE;
        uint32_t data_offset = BLOCK_HEADER_SIZE;   // skip the BlockData header

        rtt_puts("E");
        if (staging_erase_sector(addr) != FAPI_STATUS_SUCCESS) {
            rtt_puts("!\r\nDL: erase fail s=");
            rtt_put_hex8((uint8_t)block_id);
            rtt_puts("\r\n");
            return false;
        }

        rtt_puts("P");
        if (staging_program(&bw_buf[data_offset], addr, data_len) != FAPI_STATUS_SUCCESS) {
            rtt_puts("!\r\nDL: prog fail s=");
            rtt_put_hex8((uint8_t)block_id);
            rtt_puts("\r\n");
            return false;
        }

        rtt_puts("V");
        vims_invalidate();
        if (!staging_verify(addr, &bw_buf[data_offset], data_len)) {
            rtt_puts("!\r\nDL: verify fail s=");
            rtt_put_hex8((uint8_t)block_id);
            rtt_puts("\r\n");
            return false;
        }

        rtt_puts("+ ");
    }
    return true;
}

// Flash primitives for callers outside this file: the image path stages a
// compressed picture, and its decoded first plane, the same way firmware is.
uint32_t oepl_flash_erase_sector(uint32_t addr) { return staging_erase_sector(addr); }
uint32_t oepl_flash_program(const uint8_t *data, uint32_t addr, uint32_t len)
{
    return staging_program((uint8_t *)data, addr, len);
}
void oepl_flash_cache_invalidate(void) { vims_invalidate(); }

// --- Main OTA orchestrator ---

void oepl_ota_download_and_apply(struct AvailDataInfo *info)
{
    uint32_t fw_size = info->dataSize;

    rtt_puts("OTA: size=");
    rtt_put_hex32(fw_size);
    rtt_puts("\r\n");

    // Sanity checks
    if (fw_size == 0) {
        rtt_puts("OTA: empty\r\n");
        return;
    }
    if (fw_size > OTA_STAGING_SIZE) {
        rtt_puts("OTA: too large\r\n");
        return;
    }

    // Don't start on a weak supply: the download costs far more charge than a
    // check-in, and the apply at the end of it is the one step that can brick
    // the tag. The AP re-offers the update while the versions differ, so a tag
    // that waits loses nothing. (Checked again just before the apply, in case
    // the supply sags during the download.)
    uint16_t bat_mv = 0;
    if (oepl_hw_get_voltage(&bat_mv) && bat_mv < OTA_APPLY_MIN_MV) {
        rtt_puts("OTA: battery too low to start (");
        rtt_put_hex32(bat_mv);
        rtt_puts(" mV)\r\n");
        return;
    }

    if (!oepl_ota_stage_download(info, OTA_STAGING_ADDR, OTA_STAGING_SIZE)) return;

    // Final integrity check: verify the vector table in staging.
    // Valid ARM Cortex-M firmware has its initial SP inside SRAM and a reset
    // vector that points at Thumb code inside the image we just staged — a
    // vector with bit 0 clear HardFaults before a single instruction runs, and
    // one past fw_size points into flash this image never programmed.
    vims_invalidate();
    const uint32_t *staging_vectors = (const uint32_t *)OTA_STAGING_ADDR;
    uint32_t sp_val = staging_vectors[0];
    uint32_t reset_val = staging_vectors[1];
    if (sp_val < 0x20000000 || sp_val > 0x20005000 ||
        (reset_val & 1) == 0 || (reset_val & ~1u) >= fw_size) {
        rtt_puts("\r\nOTA: bad vector table! SP=");
        rtt_put_hex32(sp_val);
        rtt_puts(" RST=");
        rtt_put_hex32(reset_val);
        rtt_puts("\r\n");
        return;
    }

    rtt_puts("\r\nOTA: all blocks OK, vectors valid\r\n");

    // Send XferComplete to AP before applying (acked; if it never gets
    // through the AP will re-offer and oepl_ota_already_applied() answers it)
    rtt_puts(oepl_radio_send_xfer_complete() ? "OTA: XferComplete ACKed\r\n"
                                             : "OTA: XferComplete not acked\r\n");

    // The apply is the one irreversible step: from the first erase there is no
    // code left to return to. Shed the radio load first (the RF core is still
    // powered from the XferComplete) and refuse to start on a weak supply —
    // staging survives a reboot, so a low tag simply retries on a later
    // check-in instead of browning out mid-erase with half an image in flash.
    oepl_rf_shutdown();

    bat_mv = 0;
    if (oepl_hw_get_voltage(&bat_mv) && bat_mv < OTA_APPLY_MIN_MV) {
        rtt_puts("OTA: battery too low to apply (");
        rtt_put_hex32(bat_mv);
        rtt_puts(" mV), retry later\r\n");
        return;
    }

    // Apply OTA: copy staging to active area and reboot.
    // Cache off for the duration: apply_ota reads its own flash writes back.
    rtt_puts("OTA: APPLYING...\r\n");
    oepl_hw_watchdog_feed();
    VIMSModeSafeSet(VIMS_BASE, VIMS_MODE_OFF, true);
    apply_ota(OTA_STAGING_ADDR, fw_size, bw_buf, info->dataVer);

    // Should not reach here — apply_ota resets the CPU
    VIMSModeSafeSet(VIMS_BASE, VIMS_MODE_ENABLED, true);
    rtt_puts("OTA: apply returned?!\r\n");
}
