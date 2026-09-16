#ifndef OEPL_RADIO_CC2630_H
#define OEPL_RADIO_CC2630_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// --- OEPL Protocol Constants ---
#define PROTO_PAN_ID            0x4447

// Packet types (from oepl-proto.h)
#define PKT_AVAIL_DATA_REQ      0xE5
#define PKT_AVAIL_DATA_INFO     0xE6
#define PKT_BLOCK_REQUEST       0xE4
#define PKT_BLOCK_PARTIAL_REQUEST 0xE7  // re-request missing parts of the block the AP has buffered
#define PKT_BLOCK_PART          0xE8
#define PKT_BLOCK_REQUEST_ACK   0xE9
#define PKT_XFER_COMPLETE       0xEA
#define PKT_XFER_COMPLETE_ACK   0xEB
#define PKT_CANCEL_XFER         0xEC
#define PKT_PING                0xED
#define PKT_PONG                0xEE

// XferComplete: wait this long for the AP's XferCompleteAck, this many tries
#define XFER_COMPLETE_ACK_TIMEOUT_MS  300
#define XFER_COMPLETE_TRIES           5

// Hardware type for 6" BWR (from oepl-definitions.h)
#define HW_TYPE                 0x35  // SOLUM_M3_BWR_60

// Block transfer constants
#define BLOCK_PART_DATA_SIZE    99
#define BLOCK_MAX_PARTS         42
#define BLOCK_DATA_SIZE         4096UL
#define BLOCK_HEADER_SIZE       4       // struct BlockData header (size + checksum)
#define BLOCK_XFER_BUFFER_SIZE  (BLOCK_DATA_SIZE + BLOCK_HEADER_SIZE)  // 4100
#define BLOCK_REQ_PARTS_BYTES   6

// Block reception timeouts (see oepl_radio_request_block)
//
// APs differ a lot in how fast they pace BlockParts: a C6-radio AP has been
// observed at ~350ms between parts (42 parts ≈ 15s), others send them ~4ms
// apart. The RX window must fit a whole slow block, and the idle timeout must
// comfortably exceed the slowest inter-part gap, or a block never completes.
#define BLOCK_RX_WINDOW_MS      40000   // hard cap on one block request's RX session
#define BLOCK_ACK_TIMEOUT_MS    2000    // no ACK/parts at all after request → AP not serving
#define BLOCK_PART_TIMEOUT_MS   2000    // silence after ACK's pleaseWaitMs / between parts → burst over
// The AP always transmits a full 42-frame burst, cycling whichever parts were
// requested, and can't hear a new request while it does. Once a block is
// complete, keep listening until the burst goes quiet before returning, or
// the next block's request is lost (seen on every block on the bench: no ACK,
// the tail of the old burst, a 2 s timeout, then a retry).
#define BLOCK_DRAIN_QUIET_MS    60      // AP part spacing is ~4-6 ms
#define BLOCK_DRAIN_MAX_MS      600     // a whole burst is ~250 ms

// Wakeup reasons
#define WAKEUP_REASON_TIMED         0
#define WAKEUP_REASON_SPLASH        0xFB
#define WAKEUP_REASON_FIRSTBOOT     0xFC
#define WAKEUP_REASON_NETWORK_SCAN  0xFD
#define WAKEUP_REASON_WDT_RESET     0xFE   // reported after a HardFault reset

// Firmware version as reported to the AP (`ver` in its tag DB). Keep in step
// with the "FW vX.Y" string in splash.c. DIAG builds set bit 15 so a debug
// build (which replaces telemetry with diagnostics) is obvious at the AP.
#if defined(DIAG_TELEMETRY)
#define TAG_FW_VERSION  (0x8000 | 0x0014)
#else
#define TAG_FW_VERSION  0x0014
#endif

// Capabilities
#define CAPABILITY_SUPPORTS_COMPRESSION  0x02

// Data types
#define DATATYPE_NOUPDATE       0x00
#define DATATYPE_FW_UPDATE      0x03

// --- Protocol Structs (packed, little-endian on wire) ---

struct __attribute__((packed)) MacFrameBcast {
    uint8_t fcs[2];       // {0x01, 0xC8} for broadcast
    uint8_t seq;
    uint16_t dstPan;      // PROTO_PAN_ID (LE)
    uint16_t dstAddr;     // 0xFFFF (broadcast)
    uint16_t srcPan;      // PROTO_PAN_ID (LE)
    uint8_t src[8];       // tag MAC (8 bytes)
};  // 17 bytes

struct __attribute__((packed)) MacFrameNormal {
    uint8_t fcs[2];       // {0x41, 0xCC} for unicast
    uint8_t seq;
    uint16_t pan;         // PROTO_PAN_ID (LE)
    uint8_t dst[8];       // destination MAC
    uint8_t src[8];       // source MAC
};  // 21 bytes

struct __attribute__((packed)) AvailDataReq {
    uint8_t checksum;
    uint8_t lastPacketLQI;
    int8_t  lastPacketRSSI;
    int8_t  temperature;
    uint16_t batteryMv;
    uint8_t hwType;
    uint8_t wakeupReason;
    uint8_t capabilities;
    uint16_t tagSoftwareVersion;
    uint8_t currentChannel;
    uint8_t customMode;
    uint8_t reserved[8];
};  // 21 bytes

struct __attribute__((packed)) AvailDataInfo {
    uint8_t checksum;
    uint64_t dataVer;
    uint32_t dataSize;
    uint8_t dataType;
    uint8_t dataTypeArgument;
    uint16_t nextCheckIn;
};  // 17 bytes

struct __attribute__((packed)) BlockRequest {
    uint8_t checksum;
    uint64_t ver;
    uint8_t blockId;
    uint8_t type;
    uint8_t requestedParts[BLOCK_REQ_PARTS_BYTES];
};  // 17 bytes

struct __attribute__((packed)) BlockRequestAck {
    uint8_t checksum;
    uint16_t pleaseWaitMs;
};  // 3 bytes

struct __attribute__((packed)) BlockPart {
    uint8_t checksum;
    uint8_t blockId;
    uint8_t blockPart;
    uint8_t data[BLOCK_PART_DATA_SIZE];
};  // 102 bytes

struct __attribute__((packed)) BlockData {
    uint16_t size;
    uint16_t checksum;
    uint8_t data[];
};  // 4 bytes header

// --- Radio State ---
typedef struct {
    uint8_t mac[8];
    uint8_t ap_mac[8];
    uint8_t current_channel;     // OEPL channel index (0-5)
    uint8_t current_ieee_ch;     // IEEE channel (11,15,20,25,26,27)
    int8_t  last_rssi;
    uint8_t last_lqi;
    uint8_t seq;
    bool    ap_found;
} radio_state_t;

// --- Public API ---

// Initialize protocol layer (call after oepl_rf_init)
void oepl_radio_init(void);

// Scan channels for AP by sending PING, listening for PONG
// Returns OEPL channel index (0-5) or -1 if no AP found
int8_t oepl_radio_scan_channels(void);

// Send AvailDataReq on current channel, wait for AvailDataInfo response
// Returns true if AP responded with data info
bool oepl_radio_checkin(struct AvailDataInfo *out_info);

// Send BlockRequest, receive block parts
// parts_rcvd is in/out bitmap — accumulates across retries
// Returns number of parts received so far (caller checks >= BLOCK_MAX_PARTS)
uint8_t oepl_radio_request_block(uint8_t block_id, uint64_t data_ver, uint8_t data_type,
                                  uint8_t *block_buf, uint8_t *parts_rcvd);

// Send XferComplete, retrying until the AP acknowledges it.
// Returns true if XferCompleteAck was received.
bool oepl_radio_send_xfer_complete(void);

// Get radio state
radio_state_t *oepl_radio_get_state(void);

// Set wakeup reason for next checkin
void oepl_radio_set_wakeup_reason(uint8_t reason);

// One-shot crash report: the next AvailDataReq carries the fault PC and status
// in fields the AP stores verbatim (see DEVELOPMENT.md "Reading a crash"):
//   batteryMv   = PC[15:0]      temperature = PC[23:16]
//   LQI         = UFSR[3:0]<<4 | BFSR[3:0]   (from CFSR)
// Normal values resume on the following checkin.
void oepl_radio_set_fault_report(uint32_t pc, uint32_t cfsr);

#ifdef DIAG_TELEMETRY
// Debug builds only (make DIAG=1): the next checkin reports the last image
// download instead of telemetry —
//   LQI         = failed_blocks<<4 | xfer   (xfer: 0 not sent, 1-5 acked on
//                 that try, 0xE every TX failed, 0xF sent but never acked)
//   temperature = total block requests (capped at 127)
//   batteryMv   = rf_crc_errors<<8 | rf_buf_full   (RF core counters, summed)
void oepl_radio_set_diag_report(uint8_t failed_blocks, uint8_t xfer,
                                uint8_t requests, uint8_t rf_nok, uint8_t rf_full);
// Running totals kept by oepl_radio_request_block(); caller resets.
extern uint16_t g_diag_requests, g_diag_rf_nok, g_diag_rf_full;
extern uint8_t g_xfer_last_try;   // 1-based try on which XferComplete was acked
extern bool g_xfer_tx_ok;         // at least one XferComplete TX succeeded
#endif

// Download a single block into buf (with retries). Defined in main.c.
bool download_block(uint8_t block_id, struct AvailDataInfo *info,
                    uint8_t *buf, uint16_t *out_size);

// Block transfer buffer (4100 bytes in main.c), shared with OTA
extern uint8_t bw_buf[];

#endif // OEPL_RADIO_CC2630_H
