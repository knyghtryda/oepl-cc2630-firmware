// -----------------------------------------------------------------------------
//     CC2630 OEPL Tag Firmware
// -----------------------------------------------------------------------------
// Full OEPL tag: scan → checkin → block download → display
// Uses SEGGER RTT for debug output via J-Link.
// -----------------------------------------------------------------------------

#include <stdint.h>
#include <string.h>
#include "rtt.h"
#include "oepl_rf_cc2630.h"
#include "oepl_radio_cc2630.h"
#include "oepl_hw_abstraction_cc2630.h"
#include "drivers/oepl_display_driver_uc8159_600x448.h"
#include "splash.h"
#include "oepl_ota_cc2630.h"
#include "fault.h"

// TI driverlib
#include "sys_ctrl.h"

// PRCM registers (now from driverlib includes via sys_ctrl.h)
#include "prcm.h"
#include "hw_memmap.h"
#include "hw_prcm.h"

// RTC-timed sleep
#include "aon_rtc.h"

// Deep sleep: AON wakeup controller, event routing, interrupts, IO latch
#include "aon_wuc.h"
#include "aon_event.h"
#include "interrupt.h"
#include "hw_ints.h"
#include "aon_ioc.h"
#include "ioc.h"
#include "gpio.h"
#include "aux_wuc.h"
#include "osc.h"
#include "vims.h"
#include "ddi.h"
#include "hw_ddi_0_osc.h"
#include "hw_aon_wuc.h"
#include "watchdog.h"

// Block buffers for image download (4100 bytes each: 4-byte header + 4096 data)
// bw_buf: cached B/W block (also used by OTA), red_buf: cached Red block
uint8_t bw_buf[BLOCK_XFER_BUFFER_SIZE];
static uint8_t red_buf[BLOCK_XFER_BUFFER_SIZE];
static int8_t bw_cache_id, red_cache_id;  // which block ID is cached (-1 = none)

// Warm boot detection: .noinit survives WFI return path (debug tag with JLink).
// Hardware reset source covers full standby wakeup (battery tag, no debugger).
#define WARMBOOT_MAGIC_A 0xDEE5BEEF
#define WARMBOOT_MAGIC_B 0x2116CAFE
__attribute__((section(".noinit"))) static volatile uint32_t warmboot_magic_a;
__attribute__((section(".noinit"))) static volatile uint32_t warmboot_magic_b;

static void delay_cycles(volatile uint32_t n)
{
    while (n--) {
        if ((n & 0xFFFF) == 0) oepl_hw_wdt_kick();
        __asm volatile ("nop");
    }
}

// AON RTC interrupt handler — wakes CPU from deep sleep (WFI).
// Overrides the weak alias to Default_Handler in startup_cc2630.c.
// Without this ISR and IntEnable(INT_AON_RTC_COMB), WFI hangs forever
// because the Cortex-M3 requires an NVIC-enabled interrupt to wake.
void AON_RTC_Handler(void)
{
    AONRTCEventClear(AON_RTC_CH0 | AON_RTC_CH2);   // CH2: idle tick (oepl_hw_idle)
}

// Enter standby with RTC-timed wakeup.
//
// Sequence follows TI's PowerCC26XX.c Power_sleep() step for step: IOs
// frozen, crystal off, AUX allowed to sleep, RF/serial/peripheral/CPU domains
// off, uLDO requested, VIMS cache off, recharge configured, SLEEPDEEP + WFI.
// Execution resumes after PRCMDeepSleep() on wakeup; the wake path undoes it
// in TI's order (AUX must be back on before anything touches OSC/DDI).
//
// Measured on the bench (PPK2, debugger detached): the previous version,
// which powered off PERIPH only, sat at ~1.8 mA while "asleep".
//
// With a debugger attached the JTAG domain stays on, which prevents standby:
// WFI then simply returns and the wake path runs anyway.
static bool debugger_attached(void)
{
    return (HWREG(0xE000EDF0) & 0x1) != 0;   // CoreDebug DHCSR.C_DEBUGEN
}

// Once SCLK_LF runs from the 32 kHz crystal, bypass the LF clock qualifiers
// (as TI's PowerCC26XX driver does). The qualifier measures the LF clock
// against the HF oscillator, which keeps HF running and blocks standby:
// measured ~1.2 mA "asleep" with them active.
static bool lf_qualifiers_bypassed;
#ifdef DIAG_SLEEP_ONLY
__attribute__((section(".noinit"), used)) static uint32_t sleep_snap[16];
#endif
static void lf_clock_finalize(void)
{
    if (lf_qualifiers_bypassed) return;
    if (OSCClockSourceGet(OSC_SRC_CLK_LF) != OSC_XOSC_LF) return;   // not switched yet
    DDI16BitfieldWrite(AUX_DDI0_OSC_BASE, DDI_0_OSC_O_CTL0,
                       DDI_0_OSC_CTL0_BYPASS_XOSC_LF_CLK_QUAL_M |
                       DDI_0_OSC_CTL0_BYPASS_RCOSC_LF_CLK_QUAL_M,
                       DDI_0_OSC_CTL0_BYPASS_RCOSC_LF_CLK_QUAL_S, 0x3);
    OSCClockLossEventEnable();
    lf_qualifiers_bypassed = true;
    rtt_puts("LF clock on XOSC_LF, qualifiers bypassed\r\n");
}

static void enter_sleep(uint32_t seconds)
{
    lf_clock_finalize();

    rtt_puts("SLEEP ");
    rtt_put_hex8((seconds >> 8) & 0xFF);
    rtt_put_hex8(seconds & 0xFF);
    rtt_puts("s\r\n");

    // Shut down RF core (main loop will re-init after wakeup)
    oepl_rf_shutdown();

    // Idle tick off, or it would wake the tag out of standby every 2 ms
    oepl_hw_idle_tick(false);

    // Configure RTC wakeup (16.16 fixed-point format: seconds in upper 16 bits)
    AONRTCEnable();
    uint32_t now = AONRTCCurrentCompareValueGet();
    uint32_t target = now + (seconds << 16);
    AONRTCCompareValueSet(AON_RTC_CH0, target);
    AONRTCChannelEnable(AON_RTC_CH0);
    AONRTCEventClear(AON_RTC_CH0);

    // Combined event drives INT_AON_RTC_COMB; without it WFI never wakes.
    AONRTCCombinedEventConfig(AON_RTC_CH0);
    IntPendClear(INT_AON_RTC_COMB);
    IntEnable(INT_AON_RTC_COMB);

    // Save warm boot magic (covers a fault/reset during or after sleep)
    warmboot_magic_a = WARMBOOT_MAGIC_A;
    warmboot_magic_b = WARMBOOT_MAGIC_B;

    // Route RTC CH0 to MCU wakeup event
    AONEventMcuWakeUpSet(AON_EVENT_MCU_WU0, AON_EVENT_RTC_CH0);

    bool dbg = debugger_attached();
    rtt_puts(dbg ? "SLEEP WFI (debugger: no standby)\r\n" : "SLEEP standby\r\n");
    // --- no RTT/peripheral/AUX access from here until the wake path restores it ---

    // Step numbers follow TI's PowerCC26XX.c Power_sleep() (cc26x0).
#ifdef DIAG_SLEEP_ONLY
    sleep_snap[1] = HWREG(AUX_DDI0_OSC_BASE + DDI_0_OSC_O_CTL0);   // AUX still on here
#endif
    // 1. Freeze the IOs on the boundary between MCU and AON
    AONIOCFreezeEnable();
    // 2. If XOSC_HF is active, force it off (RF init switches it back on)
    if (OSCClockSourceGet(OSC_SRC_CLK_HF) == OSC_XOSC_HF) {
        OSCHF_SwitchToRcOscTurnOffXosc();
    }
    // 3. Allow AUX to power down (it is forced on at boot and after each
    //    wake; a forced-on AUX blocks standby -- read back as AUXCTL=1)
    AONWUCAuxWakeupEvent(AONWUC_AUX_ALLOW_SLEEP);
    // 4. Make sure writes take effect
    SysCtrlAonSync();
    // 7. Clock settings take effect
    PRCMLoadSet();
    // 8. Request power off of the MCU-voltage-domain domains
    PRCMPowerDomainOff(PRCM_DOMAIN_RFCORE | PRCM_DOMAIN_SERIAL |
                       PRCM_DOMAIN_PERIPH | PRCM_DOMAIN_CPU);
    // 9. Request uLDO during standby (VDCTL.ULDO; without it the chip only
    //    reaches deep sleep on the main regulator, ~1 mA measured)
    PRCMMcuUldoConfigure(true);
    // 10. VIMS cache off and not retained
    uint32_t modeVIMS;
    do { modeVIMS = VIMSModeGet(VIMS_BASE); } while (modeVIMS == VIMS_MODE_CHANGING);
    if (modeVIMS == VIMS_MODE_ENABLED) VIMSModeSet(VIMS_BASE, VIMS_MODE_OFF);
    PRCMCacheRetentionDisable();
    // 11. Recharge parameters
    SysCtrlSetRechargeBeforePowerDown(XOSC_IN_HIGH_POWER_MODE);
    // 12. Make sure all writes have taken effect
    SysCtrlAonSync();

#ifdef DIAG_SLEEP_ONLY
    // Snapshot of the power configuration at the moment of sleep (noinit)
    sleep_snap[0]++;
    sleep_snap[3] = HWREG(AON_WUC_BASE + AON_WUC_O_PWRSTAT);
    sleep_snap[4] = HWREG(AON_WUC_BASE + AON_WUC_O_CTL0);
    sleep_snap[5] = HWREG(AON_WUC_BASE + AON_WUC_O_JTAGCFG);
    sleep_snap[7] = HWREG(PRCM_BASE + PRCM_O_PDSTAT0);
    sleep_snap[8] = HWREG(PRCM_BASE + PRCM_O_PDSTAT1);
    sleep_snap[10] = HWREG(PRCM_BASE + PRCM_O_PDCTL1);
    sleep_snap[11] = HWREG(AON_WUC_BASE + AON_WUC_O_AUXCTL);
    sleep_snap[14] = HWREG(PRCM_BASE + PRCM_O_VDCTL);
    sleep_snap[15] = 0x5AFE5AFE;
#endif
    (void)dbg;

    // 13. Invoke deep sleep to go to STANDBY
    PRCMDeepSleep();

    // --- Wake ---
    // 14. Restore VIMS and cache retention
    if (modeVIMS == VIMS_MODE_ENABLED) VIMSModeSet(VIMS_BASE, modeVIMS);
    PRCMCacheRetentionEnable();
    // 15. Start forcing power to AUX
    AONWUCAuxWakeupEvent(AONWUC_AUX_WAKEUP);
    // 16. Re-power the peripheral domains we use (RF core stays off until RF init)
    PRCMPowerDomainOn(PRCM_DOMAIN_SERIAL | PRCM_DOMAIN_PERIPH);
    // 18. Clock settings take effect
    PRCMLoadSet();
    // 19. Release request for uLDO
    PRCMMcuUldoConfigure(false);
    // 21. Wait until the domains are back on
    for (volatile uint32_t i = 0; i < 2000000; i++)
        if (PRCMPowerDomainStatus(PRCM_DOMAIN_SERIAL | PRCM_DOMAIN_PERIPH) == PRCM_DOMAIN_POWER_ON)
            break;
    // 22. RTC shadow values up to date
    SysCtrlAonSync();
    // 24. Disable IO freeze
    AONIOCFreezeDisable();
    SysCtrlAonSync();
    // 25. Wait for AUX to power up (everything OSC/DDI-related needs it)
    for (volatile uint32_t i = 0; i < 5000000; i++)
        if (AONWUCPowerStatusGet() & AONWUC_AUX_POWER_ON) break;
    SysCtrlAdjustRechargeAfterPowerDown(0);

    warmboot_magic_a = 0;
    warmboot_magic_b = 0;
    IntDisable(INT_AON_RTC_COMB);
    IntPendClear(INT_AON_RTC_COMB);
    AONRTCEventClear(AON_RTC_CH0);

    // GPIO clock back on
    PRCMPeripheralRunEnable(PRCM_PERIPH_GPIO);
    PRCMLoadSet();
    for (volatile uint32_t i = 0; i < 500000; i++)
        if (PRCMLoadGet()) break;

    rtt_puts("WAKE\r\n");
}

static void print_mac_msb(const uint8_t *mac_lsb)
{
    // Print MAC in human-readable MSB-first order (reverse of wire order)
    for (int i = 7; i >= 0; i--) {
        rtt_put_hex8(mac_lsb[i]);
        if (i > 0) rtt_puts(":");
    }
}

static bool do_scan_and_checkin(struct AvailDataInfo *info)
{
    // Scan for AP
    rtt_puts("\r\n--- SCAN ---\r\n");
    int8_t ch = oepl_radio_scan_channels();
    if (ch < 0) {
        // Try direct checkin on all channels as fallback
        rtt_puts("Direct checkin...\r\n");
        for (uint8_t c = 0; c < OEPL_NUM_CHANNELS; c++) {
            rf_status_t rc = oepl_rf_set_channel(c);
            if (rc != RF_OK) continue;

            uint8_t ieee_ch = oepl_channel_map[c];
            radio_state_t *rst = oepl_radio_get_state();
            rst->current_channel = c;
            rst->current_ieee_ch = ieee_ch;
            rst->ap_found = true;
            memset(rst->ap_mac, 0xFF, 8);

            rtt_puts("Ch ");
            rtt_put_hex8(ieee_ch);
            rtt_puts(": ");
            if (oepl_radio_checkin(info)) return true;
        }
        return false;
    }

    // AP found, do checkin
    rtt_puts("AP found, checkin...\r\n");
    return oepl_radio_checkin(info);
}

// Check the BlockData header the AP prepends to each block: data length and
// a 16-bit sum of the data bytes. This catches a known AP failure mode where
// the C6 radio's request to the ESP32 is dropped and it serves the previous
// block's contents relabelled with the new block id.
static bool block_checksum_ok(const uint8_t *buf, uint32_t expected_len)
{
    const struct BlockData *bd = (const struct BlockData *)buf;
    if (bd->size != expected_len) return false;
    uint16_t sum = 0;
    for (uint32_t i = 0; i < expected_len; i++)
        sum += buf[BLOCK_HEADER_SIZE + i];
    return sum == bd->checksum;
}

// Download a specific block into a buffer, with retries
// Accumulates parts across attempts — missing parts requested on retry
bool download_block(uint8_t block_id, struct AvailDataInfo *info,
                    uint8_t *buf, uint16_t *out_size)
{
    rtt_puts("B");
    rtt_put_hex8(block_id);

    uint8_t parts_rcvd[BLOCK_REQ_PARTS_BYTES];
    memset(parts_rcvd, 0, sizeof(parts_rcvd));
    memset(buf, 0x00, BLOCK_XFER_BUFFER_SIZE);

    uint32_t remaining = info->dataSize - (uint32_t)block_id * BLOCK_DATA_SIZE;
    uint32_t data_len = (remaining > BLOCK_DATA_SIZE) ? BLOCK_DATA_SIZE : remaining;

    uint8_t zero_count = 0;  // consecutive attempts with 0 parts received
    for (uint8_t attempt = 0; attempt < 15; attempt++) {
        if (attempt > 0) {
            rtt_puts("R");
            oepl_hw_delay_ms(100);
        }
        uint8_t got = oepl_radio_request_block(block_id, info->dataVer, info->dataType,
                                                buf, parts_rcvd);
        // Accept 41/42 only after 8 attempts
        bool complete = (got >= BLOCK_MAX_PARTS) ||
                        (got >= BLOCK_MAX_PARTS - 1 && attempt >= 7);
        if (complete) {
            if (block_checksum_ok(buf, data_len)) {
                rtt_puts(got >= BLOCK_MAX_PARTS ? "+" : "~");
                *out_size = BLOCK_XFER_BUFFER_SIZE;
                return true;
            }
            // Wrong contents: start over with a full (forced) request so the
            // AP re-fetches the block rather than resending its buffer.
            rtt_puts("C!");
            memset(parts_rcvd, 0, sizeof(parts_rcvd));
            continue;
        }
        // If AP isn't responding at all (0 parts, 3 times), give up early
        if (got == 0) {
            zero_count++;
            if (zero_count >= 3) {
                rtt_puts("X");
                break;
            }
        } else {
            zero_count = 0;
        }
    }
    rtt_puts("!");
    return false;
}

// Ensure a block is in the B/W cache.
// On download failure, fills buffer with white (0x00) and caches the block ID
// to avoid re-attempting the same failed block on every row.
static bool ensure_bw_block(uint8_t block_id, struct AvailDataInfo *info)
{
    if (bw_cache_id == block_id) return true;
    uint16_t sz;
    if (!download_block(block_id, info, bw_buf, &sz)) {
        memset(bw_buf, 0x00, BLOCK_XFER_BUFFER_SIZE);
        bw_cache_id = block_id;
        return false;
    }
    bw_cache_id = block_id;
    return true;
}

// Ensure a block is in the Red cache.
// On download failure, fills buffer with 0x00 (no red) and caches.
static bool ensure_red_block(uint8_t block_id, struct AvailDataInfo *info)
{
    if (red_cache_id == block_id) return true;
    uint16_t sz;
    if (!download_block(block_id, info, red_buf, &sz)) {
        memset(red_buf, 0x00, BLOCK_XFER_BUFFER_SIZE);
        red_cache_id = block_id;
        return false;
    }
    red_cache_id = block_id;
    return true;
}

// Count of blocks that failed download (reset before each image)
static uint8_t dl_failed_blocks;

// Get bytes from the image at a given offset, using cached blocks.
// Each block has a 4-byte BlockData header (size + checksum) followed by
// BLOCK_DATA_SIZE bytes of actual image data. We skip the header.
// On block download failure, uses white data (ensure_*_block fills buffer).
static void get_image_bytes(uint32_t offset, uint8_t *out, uint16_t len,
                             struct AvailDataInfo *info, bool is_red_plane)
{
    while (len > 0) {
        // If too many blocks have failed, AP has nothing — fill white and stop
        if (dl_failed_blocks >= 3) {
            memset(out, 0x00, len);
            return;
        }

        uint8_t block_id = (uint8_t)(offset / BLOCK_DATA_SIZE);
        uint16_t block_off = (uint16_t)(offset % BLOCK_DATA_SIZE);
        uint16_t avail = (uint16_t)BLOCK_DATA_SIZE - block_off;
        if (avail > len) avail = len;

        uint8_t *cache;
        if (is_red_plane) {
            if (!ensure_red_block(block_id, info)) dl_failed_blocks++;
            cache = red_buf;
        } else {
            if (!ensure_bw_block(block_id, info)) dl_failed_blocks++;
            cache = bw_buf;
        }

        // Skip BLOCK_HEADER_SIZE (4 bytes) at start of each block
        memcpy(out, &cache[BLOCK_HEADER_SIZE + block_off], avail);
        out += avail;
        offset += avail;
        len -= avail;
    }
}

// Convert 1 byte B/W + 1 byte Red (8 pixels) to 4 bytes of 4bpp UC8159
// B/W: bit=1 → black, bit=0 → white. Red: bit=1 → red (overrides B/W)
// UC8159 4bpp: 0x0=black, 0x3=white, 0x4=red
static void bwr_to_4bpp(uint8_t bw, uint8_t red, uint8_t out[4])
{
    for (uint8_t i = 0; i < 4; i++) {
        uint8_t hi_bw  = (bw >> 7) & 1;
        uint8_t hi_red = (red >> 7) & 1;
        uint8_t lo_bw  = (bw >> 6) & 1;
        uint8_t lo_red = (red >> 6) & 1;

        uint8_t hi_nib = hi_red ? 0x4 : (hi_bw ? 0x0 : 0x3);
        uint8_t lo_nib = lo_red ? 0x4 : (lo_bw ? 0x0 : 0x3);

        out[i] = (hi_nib << 4) | lo_nib;
        bw <<= 2;
        red <<= 2;
    }
}

// Download image and stream to display.
// On block download failure, fills with white and continues instead of aborting.
// Returns true if image was displayed (even partially).
static bool download_and_display(struct AvailDataInfo *info)
{
    uint32_t data_size = info->dataSize;
    uint8_t data_type = info->dataType;
    uint16_t width = DISPLAY_WIDTH_600X448;
    uint16_t height = DISPLAY_HEIGHT_600X448;
    uint16_t row_bytes = width / 8;           // 75 bytes per 1bpp row
    uint32_t plane_size = (uint32_t)row_bytes * height;  // 33,600 bytes
    bool has_red = (data_type == 0x21) && (data_size >= plane_size * 2);
    dl_failed_blocks = 0;
#ifdef DIAG_TELEMETRY
    g_diag_requests = g_diag_rf_nok = g_diag_rf_full = 0;
    uint8_t diag_xfer = 0;
#endif

    rtt_puts("DL+DISP: sz=");
    rtt_put_hex32(data_size);
    rtt_puts(" t=");
    rtt_put_hex8(data_type);
    rtt_puts(has_red ? " BWR" : " BW");
    rtt_puts("\r\n");

    bw_cache_id = -1;
    red_cache_id = -1;

    // Full re-init display before each update (UC8159 requires fresh init before each DRF)
    rtt_puts("EPD wake...");
    uc8159_wake();
    rtt_puts("OK\r\n");

    // Open display for pixel data: DTM1 (cmd 0x10) in one CS frame
    oepl_hw_gpio_set(15, false);  // DC = command
    oepl_hw_spi_cs_assert();
    { uint8_t c = 0x10; oepl_hw_spi_send_raw(&c, 1); }
    oepl_hw_gpio_set(15, true);   // DC = data

    // Stream rows to display
    uint8_t bw_line[75];
    uint8_t red_line[75];
    uint8_t row_4bpp[300];

    for (uint16_t y = 0; y < height; y++) {
        // Abort early if AP clearly has nothing to serve
        if (dl_failed_blocks >= 3) {
            rtt_puts("\r\nABORT: AP not serving data\r\n");
            break;   // no refresh will follow (see below), so stop streaming
        }

        uint32_t bw_offset = (uint32_t)y * row_bytes;

        // Get B/W line (failed blocks auto-fill white via ensure_bw_block)
        get_image_bytes(bw_offset, bw_line, row_bytes, info, false);

        // Get Red line (if 2bpp)
        if (has_red) {
            uint32_t red_offset = plane_size + (uint32_t)y * row_bytes;
            get_image_bytes(red_offset, red_line, row_bytes, info, true);
        } else {
            memset(red_line, 0, row_bytes);
        }

        // Convert to 4bpp (GD bit handles orientation)
        for (uint8_t x = 0; x < row_bytes; x++) {
            bwr_to_4bpp(bw_line[x], red_line[x], &row_4bpp[x * 4]);
        }

        // Send row to display
        oepl_hw_spi_send_raw(row_4bpp, 300);

        // Progress every 64 rows
        if ((y & 0x3F) == 0) {
            rtt_puts(".");
        }
    }

    oepl_hw_spi_cs_deassert();

    if (dl_failed_blocks > 0) {
        // Incomplete image: don't refresh. The panel keeps showing the last
        // good image (its RAM now holds the partial one, but nothing is
        // displayed until DRF), no XferComplete is sent, and the AP re-offers
        // the image at the next check-in. Refreshing here used to paint the
        // missing blocks white -- a whole white panel when the radio died
        // on the first block.
        rtt_puts("\r\nDATA PARTIAL (");
        rtt_put_hex8(dl_failed_blocks);
        rtt_puts(" failed) -- not refreshing\r\n");
        uc8159_sleep();
#ifdef DIAG_TELEMETRY
        oepl_radio_set_diag_report(dl_failed_blocks, 0,
                                   (uint8_t)(g_diag_requests > 255 ? 255 : g_diag_requests),
                                   (uint8_t)(g_diag_rf_nok > 255 ? 255 : g_diag_rf_nok),
                                   (uint8_t)(g_diag_rf_full > 255 ? 255 : g_diag_rf_full));
#endif
        return false;
    }
    rtt_puts("\r\nDATA OK\r\n");

    // Refresh the panel, wait it out, power it off. XferComplete must not be
    // sent until this returns: transmitting during the refresh's peak current
    // draw lost the (then unacknowledged) frame on most cycles, and the AP
    // kept re-offering the image.
    uc8159_refresh_and_sleep();

    // Only send XferComplete if download was fully successful.
    // On partial failure, AP keeps data pending for retry next checkin.
    if (dl_failed_blocks == 0) {
        if (oepl_radio_send_xfer_complete()) {
            rtt_puts("XferComplete ACKed\r\n");
#ifdef DIAG_TELEMETRY
            diag_xfer = g_xfer_last_try;
#endif
        } else {
            rtt_puts("XferComplete NOT acked (AP will re-offer)\r\n");
#ifdef DIAG_TELEMETRY
            diag_xfer = g_xfer_tx_ok ? 0xF : 0xE;
#endif
        }
    } else {
        rtt_puts("Skipping XferComplete (retry next checkin)\r\n");
    }
#ifdef DIAG_TELEMETRY
    oepl_radio_set_diag_report(dl_failed_blocks, diag_xfer,
                               (uint8_t)(g_diag_requests > 255 ? 255 : g_diag_requests),
                               (uint8_t)(g_diag_rf_nok > 255 ? 255 : g_diag_rf_nok),
                               (uint8_t)(g_diag_rf_full > 255 ? 255 : g_diag_rf_full));
#endif

    return true;
}

int main(void)
{
    // --- Init RTT first (so we can debug early) ---
    rtt_init();

    // --- Check for warm boot (wakeup from standby) ---
    // Primary: IOC latch frozen (IOCLATCH_EN==0) — set by enter_sleep() before
    //   standby, synced via SysCtrlAonSync. TI-recommended detection method.
    // Fallback: SRAM magic in .noinit (survives WFI return + fault resets).
    bool ioc_frozen = (HWREG(AON_IOC_BASE + AON_IOC_O_IOCLATCH) & AON_IOC_IOCLATCH_EN) == 0;
    bool warm_boot = ioc_frozen ||
                     (warmboot_magic_a == WARMBOOT_MAGIC_A &&
                      warmboot_magic_b == WARMBOOT_MAGIC_B);
    warmboot_magic_a = 0;
    warmboot_magic_b = 0;

    // Post-standby adjustments
    if (ioc_frozen) {
        AONIOCFreezeDisable();
        SysCtrlAonUpdate();
        SysCtrlAdjustRechargeAfterPowerDown(0);
    }

    // --- Power up PERIPH domain (for GPIO) ---
    HWREG(PRCM_BASE + PRCM_O_PDCTL0PERIPH) = 1;
    for (volatile uint32_t i = 0; i < 500000; i++)
        if (HWREG(PRCM_BASE + PRCM_O_PDSTAT0PERIPH) & 1) break;

    // --- Enable GPIO clock ---
    HWREG(PRCM_BASE + PRCM_O_GPIOCLKGR) = 0x01;
    HWREG(PRCM_NONBUF_BASE + PRCM_O_CLKLOADCTL) = 0x01;
    for (volatile uint32_t i = 0; i < 500000; i++)
        if (HWREG(PRCM_BASE + PRCM_O_CLKLOADCTL) & 0x02) break;

    if (warm_boot) {
        delay_cycles(4800000);  // ~0.1s (shorter — no RTT client to wait for)
        rtt_puts("\r\n=== CC2630 OEPL Tag (WARM) ===\r\n");
    } else {
        delay_cycles(24000000);  // ~3 seconds (wait for RTT client)
        rtt_puts("\r\n=== CC2630 OEPL Tag ===\r\n");
    }

    // --- Boot message ---
    rtt_puts("RST=");
    rtt_put_hex8((uint8_t)SysCtrlResetSourceGet());
    rtt_puts(" IOC=");
    rtt_put_hex8(ioc_frozen ? 1 : 0);
    rtt_puts(" WARM=");
    rtt_put_hex8(warm_boot ? 1 : 0);
    rtt_puts("\r\n");

    // Watchdog: from here on anything that hangs for ~90s gets reset.
    oepl_hw_wdt_init();

    // A watchdog reset is a warm reset with no fault record: synthesise one
    // so it is reported like a crash (PC 0xDEADD006 = watchdog). Standby
    // wakeups also come through reset, so only a cold boot counts.
    if (!warm_boot && SysCtrlResetSourceGet() == RSTSRC_WARMRESET &&
        g_fault.magic != FAULT_MAGIC) {
        g_fault.pc = FAULT_PC_WATCHDOG;
        g_fault.lr = g_fault.sp = g_fault.cfsr = g_fault.bfar = 0;
        g_fault.magic = FAULT_MAGIC;
    }

#ifdef DIAG_SLEEP_ONLY
    // make DIAG_SLEEP_ONLY=1: every DIO high-impedance except DIO5 (panel
    // supply) held low and DIO11 (SPI flash CS) held high, no radio, no
    // display -- just standby in a loop. The
    // PPK2 reading is then the MCU + board floor with nothing driven.
    for (uint8_t dio = 0; dio <= 30; dio++) {
        if (dio == 5 || dio == 11) continue;
        GPIO_setOutputEnableDio(dio, GPIO_OUTPUT_DISABLE);
        IOCPortConfigureSet(dio, IOC_PORT_GPIO, IOC_NO_IOPULL | IOC_INPUT_DISABLE);
    }
    IOCPinTypeGpioOutput(5);
    GPIO_setOutputEnableDio(5, GPIO_OUTPUT_ENABLE);
    GPIO_clearDio(5);   // panel supply off (high = on: +160 uA measured)
    IOCPinTypeGpioOutput(11);          // SPI flash CS: deselected
    GPIO_setOutputEnableDio(11, GPIO_OUTPUT_ENABLE);
    GPIO_setDio(11);
    oepl_hw_flash_deep_sleep();
    GPIO_setOutputEnableDio(9, GPIO_OUTPUT_DISABLE);
    GPIO_setOutputEnableDio(10, GPIO_OUTPUT_DISABLE);
    IOCPortConfigureSet(9, IOC_PORT_GPIO, IOC_NO_IOPULL | IOC_INPUT_DISABLE);
    IOCPortConfigureSet(10, IOC_PORT_GPIO, IOC_NO_IOPULL | IOC_INPUT_DISABLE);

    rtt_puts("DIAG_SLEEP_ONLY\r\n");
    while (1) enter_sleep(20);
#endif

    // Report a HardFault from the previous run (record written by the handler)
    bool had_fault = (g_fault.magic == FAULT_MAGIC);
    struct fault_record last_fault = {0};
    char fault_str[40] = "";
    if (had_fault) {
        last_fault = g_fault;
        g_fault.magic = 0;
        rtt_puts("LAST FAULT: PC="); rtt_put_hex32(last_fault.pc);
        rtt_puts(" LR="); rtt_put_hex32(last_fault.lr);
        rtt_puts(" SP="); rtt_put_hex32(last_fault.sp);
        rtt_puts(" CFSR="); rtt_put_hex32(last_fault.cfsr);
        rtt_puts(" BFAR="); rtt_put_hex32(last_fault.bfar);
        rtt_puts("\r\n");
        // Shown in red on the splash so a sealed tag's crash can be read off
        // the panel: "FAULT PC=xxxxxxxx CFSR=xxxxxxxx"
        static const char hexd[] = "0123456789ABCDEF";
        char *q = fault_str;
        memcpy(q, "FAULT PC=", 9); q += 9;
        for (int i = 28; i >= 0; i -= 4) *q++ = hexd[(last_fault.pc >> i) & 0xF];
        memcpy(q, " CFSR=", 6); q += 6;
        for (int i = 28; i >= 0; i -= 4) *q++ = hexd[(last_fault.cfsr >> i) & 0xF];
        *q = 0;
    }

    // Print MAC in human-readable form
    uint8_t mac[8];
    oepl_rf_get_mac(mac);
    rtt_puts("MAC: ");
    print_mac_msb(mac);
    rtt_puts("\r\n");

    // --- Board peripherals to their low-power state ---
    // Panel: unpowered, lines released. Don't run uc8159_init() here: it
    // powers the panel, and nothing would power it off again before standby;
    // both draw paths call uc8159_wake(). External flash: deep power-down.
    oepl_hw_gpio_init();
    oepl_hw_epd_pins_off();
    if (!warm_boot) oepl_hw_flash_deep_sleep();

    // --- Initialize RF core ---
    rf_status_t rc = oepl_rf_init();
    if (rc != RF_OK) {
        rtt_puts("RF init FAILED: ");
        rtt_put_hex8(rc);
        rtt_puts("\r\n");
        goto idle;
    }
    rtt_puts("RF init OK\r\n");

    // --- Initialize OEPL radio protocol layer ---
    oepl_radio_init();

    // --- Splash screen only on cold boot ---
    if (!warm_boot) {
        rtt_puts("SPLASH: drawing\r\n");
        oepl_radio_set_wakeup_reason(WAKEUP_REASON_SPLASH);
        int8_t splash_ch = oepl_radio_scan_channels();
        int8_t temp_c;
        uint16_t bat_mv;
        oepl_hw_get_temperature(&temp_c);
        oepl_hw_get_voltage(&bat_mv);
        radio_state_t *rst = oepl_radio_get_state();
        splash_display(mac, bat_mv, temp_c, splash_ch >= 0, rst->current_ieee_ch,
                       had_fault ? fault_str : NULL);
        rtt_puts("SPLASH: done\r\n");
    } else {
        rtt_puts("WARM BOOT: skipping splash\r\n");
    }

    // Tell the AP the first checkin after a crash is a fault reset (0xFE);
    // it shows up as wakeupReason in the AP tag DB, with the fault PC/status
    // encoded in that one checkin's battery/temperature/LQI fields.
    if (had_fault) {
        oepl_radio_set_wakeup_reason(WAKEUP_REASON_WDT_RESET);
        oepl_radio_set_fault_report(last_fault.pc, last_fault.cfsr);
    }

    // --- Main loop: periodic checkin + sleep ---
    // Standby (RTC wakeup) between every check-in.
    uint32_t checkin_count = 0;
    // Always use standby between check-ins. (Cold boots used to busy-wait
    // the first two intervals for debugging; under a debugger the WFI simply
    // returns, so that isn't needed, and it cost ~2 min at 6 mA per boot.)
    bool use_sleep = true;
    while (1) {
        rtt_puts("\r\n=== Checkin #");
        rtt_put_hex32(checkin_count);
        rtt_puts(" ===\r\n");

        struct AvailDataInfo info;
        memset(&info, 0, sizeof(info));

        bool checkin_ok = do_scan_and_checkin(&info);

        if (checkin_ok) {
            rtt_puts("Checkin OK: dataType=");
            rtt_put_hex8(info.dataType);
            rtt_puts(" nextCheckIn=");
            rtt_put_hex8((info.nextCheckIn >> 8) & 0xFF);
            rtt_put_hex8(info.nextCheckIn & 0xFF);
            rtt_puts("\r\n");

            if (info.dataType == DATATYPE_FW_UPDATE) {
                if (oepl_ota_already_applied(info.dataVer)) {
                    rtt_puts("OTA: already applied, sending XferComplete\r\n");
                    oepl_radio_send_xfer_complete();
                } else {
                    rtt_puts("*** FW UPDATE ***\r\n");
                    oepl_ota_download_and_apply(&info);
                    // Returns only on failure — will retry next checkin
                    rtt_puts("FW update failed, retry later\r\n");
                }
            } else if (info.dataType != DATATYPE_NOUPDATE) {
                if (download_and_display(&info)) {
                    rtt_puts("*** IMAGE DISPLAYED ***\r\n");
                    oepl_radio_set_wakeup_reason(WAKEUP_REASON_TIMED);
                } else {
                    rtt_puts("Display failed, will retry next checkin\r\n");
                    oepl_radio_set_wakeup_reason(WAKEUP_REASON_TIMED);
                }
            } else {
                rtt_puts("No pending data\r\n");
                oepl_radio_set_wakeup_reason(WAKEUP_REASON_TIMED);
            }

            // AP sends nextCheckIn in minutes; convert to seconds
            uint32_t wait_sec = (uint32_t)info.nextCheckIn * 60;
            if (wait_sec < 30) wait_sec = 30;
            if (wait_sec > 3600) wait_sec = 3600;

            if (use_sleep) {
                enter_sleep(wait_sec);
            } else {
                rtt_puts("Sleep ");
                rtt_put_hex8((wait_sec >> 8) & 0xFF);
                rtt_put_hex8(wait_sec & 0xFF);
                rtt_puts("s (busy)\r\n");
                for (uint32_t s = 0; s < wait_sec; s++)
                    delay_cycles(8000000);
            }
        } else {
            if (use_sleep) {
                rtt_puts("Checkin failed, retry in 30s\r\n");
                enter_sleep(30);
            } else {
                rtt_puts("Checkin failed, retry in 30s (busy)\r\n");
                for (uint32_t s = 0; s < 30; s++)
                    delay_cycles(8000000);
            }
        }

        // After sleep, RF core was shut down — re-init before next checkin
        if (use_sleep) {
            bool rf_ok = false;
            for (uint8_t retry = 0; retry < 5; retry++) {
                if (retry > 0) {
                    rtt_puts("RF re-init retry ");
                    rtt_put_hex8(retry);
                    rtt_puts("\r\n");
                    delay_cycles(48000000);  // ~1s delay between retries
                }
                rc = oepl_rf_init();
                if (rc == RF_OK) {
                    rf_ok = true;
                    break;
                }
            }
            if (!rf_ok) {
                // All retries failed — system reset instead of dying
                rtt_puts("RF re-init FAILED x5, RESET\r\n");
                // Brief delay so RTT/UART output flushes
                delay_cycles(4800000);
                // Reset via ROM Hard-API
                typedef void (*reset_fn_t)(void);
                uint32_t *hapi_table = (uint32_t *)0x10000048;
                reset_fn_t rom_reset = (reset_fn_t)hapi_table[6];
                rom_reset();
                // Should not reach here
                while (1) __asm volatile("nop");
            }
            oepl_radio_init();
        }

        checkin_count++;
    }

idle:
    // Fatal error — wait 10 seconds then reset to try again
    rtt_puts("Fatal error, reset in 10s\r\n");
    for (uint8_t i = 0; i < 10; i++)
        delay_cycles(48000000);  // ~1s each
    {
        typedef void (*reset_fn_t)(void);
        uint32_t *hapi_table = (uint32_t *)0x10000048;
        reset_fn_t rom_reset = (reset_fn_t)hapi_table[6];
        rom_reset();
    }
    while (1) __asm volatile("nop");

    return 0;
}
