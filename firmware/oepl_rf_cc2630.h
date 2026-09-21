// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nathan Bigelow
#ifndef OEPL_RF_CC2630_H
#define OEPL_RF_CC2630_H

#include <stdint.h>
#include <stdbool.h>

// IEEE 802.15.4 channel map for OEPL (6 channels)
#define OEPL_NUM_CHANNELS   6
extern const uint8_t oepl_channel_map[OEPL_NUM_CHANNELS]; // {11,15,20,25,26,27}

// RF core status codes
typedef enum {
    RF_OK = 0,
    RF_ERR_POWER,
    RF_ERR_BOOT,
    RF_ERR_SETUP,
    RF_ERR_FS,
    RF_ERR_TX,
    RF_ERR_RX,
    RF_ERR_TIMEOUT
} rf_status_t;

// Initialize and boot the RF core (power, clocks, RADIO_SETUP for IEEE 802.15.4)
rf_status_t oepl_rf_init(void);

// Tune frequency synthesizer to an OEPL channel index (0-5 -> IEEE ch 11,15,20,25,26,27)
rf_status_t oepl_rf_set_channel(uint8_t oepl_channel_idx);

// Transmit an IEEE 802.15.4 frame (payload only - PHY header and CRC added by HW)
rf_status_t oepl_rf_tx(const uint8_t *payload, uint8_t len);

// Start RX on current channel. Received frames go into internal queue.
rf_status_t oepl_rf_rx_start(uint8_t ieee_channel, uint32_t timeout_us);

// Stop ongoing RX
void oepl_rf_rx_stop(void);

// Check if a received frame is available, return pointer and length
// Returns pointer to frame data inside RX queue entry (valid until next rx_flush)
// Returns NULL if no frame available
uint8_t *oepl_rf_rx_get(uint8_t *out_len, int8_t *out_rssi);

// Release the current RX entry so it can be reused
void oepl_rf_rx_flush(void);

// Get RX command status (for diagnostics)
uint16_t oepl_rf_rx_status(void);
bool oepl_rf_rx_ended(void);       // background RX command finished (done/error)
void oepl_rf_rx_flush_all(void);   // discard all queued frames (RX must be stopped)

// The RF core stopped acknowledging the doorbell. The current radio operation
// has failed; the core is powered off before the next sleep and re-initialised
// on the next wake, which clears it. Reported to the AP once (main.c).
bool oepl_rf_hung(void);
uint32_t oepl_rf_hung_op(void);
uint8_t oepl_rf_hung_phase(void);
uint8_t oepl_rf_hung_cmdsta(void);
void oepl_rf_hung_clear(void);

// Counters from the RF core's RX output struct (reset by each oepl_rf_rx_start)
typedef struct {
    uint8_t data;      // data frames received into the queue
    uint8_t nok;       // frames dropped for CRC error
    uint8_t ignored;   // frames rejected by address filtering
    uint8_t buf_full;  // frames dropped because no RX entry was free
} rf_rx_stats_t;
void oepl_rf_rx_stats(rf_rx_stats_t *st);

// Radio timer counter, 4 MHz ticks (valid while RF core is up)
#define RF_RAT_TICKS_PER_MS 4000UL
uint32_t oepl_rf_rat_now(void);

// Power down the RF core
void oepl_rf_shutdown(void);

// Read the 8-byte IEEE MAC address from FCFG1
void oepl_rf_get_mac(uint8_t mac[8]);

#endif // OEPL_RF_CC2630_H
