// -----------------------------------------------------------------------------
//  OEPL Protocol Layer for CC2630
//  Builds IEEE 802.15.4 frames with OEPL payload, uses oepl_rf_cc2630 for TX/RX
//
//  IMPORTANT: CMD_IEEE_TX is a foreground command that requires CMD_IEEE_RX
//  to be active as the background command. So the pattern is always:
//    1. Start CMD_IEEE_RX (background)
//    2. Send CMD_IEEE_TX (foreground, returns to RX after TX)
//    3. Wait for response in RX queue
//    4. Stop RX when done
// -----------------------------------------------------------------------------

#include "oepl_radio_cc2630.h"
#include "oepl_rf_cc2630.h"
#include "oepl_hw_abstraction_cc2630.h"
#include "rf_mailbox.h"
#include "rtt.h"
#include <string.h>

// --- Static State ---
static radio_state_t radio_st;
static uint8_t tx_frame[64];
static uint8_t g_wakeup_reason = WAKEUP_REASON_FIRSTBOOT;
static bool g_fault_pending;
static bool g_fault_reported;   // went out in the frame we are waiting on
static uint8_t g_fault_class, g_fault_why;
static uint16_t g_fault_detail;
#ifdef DIAG_TELEMETRY
static bool g_diag_pending;
static bool g_diag_reported;
static uint8_t g_diag_lqi, g_diag_temp;
static uint16_t g_diag_bat;
uint16_t g_diag_requests, g_diag_rf_nok, g_diag_rf_full;
uint8_t g_xfer_last_try;
bool g_xfer_tx_ok;
#endif

// --- Helpers ---

static uint8_t mac_hdr_size(const uint8_t *pkt, uint8_t pkt_len);

static void add_crc(void *p, uint8_t len)
{
    uint8_t total = 0;
    for (uint8_t c = 1; c < len; c++)
        total += ((uint8_t *)p)[c];
    ((uint8_t *)p)[0] = total;
}

static bool check_crc(const void *p, uint8_t len)
{
    uint8_t total = 0;
    for (uint8_t c = 1; c < len; c++)
        total += ((const uint8_t *)p)[c];
    return ((const uint8_t *)p)[0] == total;
}

static void build_bcast_header(struct MacFrameBcast *f)
{
    f->fcs[0] = 0x01;  // Data frame, no ACK request (broadcast)
    f->fcs[1] = 0xC8;  // Short dst addr, long src addr, no PAN compress
    f->seq = radio_st.seq++;
    f->dstPan = PROTO_PAN_ID;
    f->dstAddr = 0xFFFF;
    f->srcPan = PROTO_PAN_ID;
    memcpy(f->src, radio_st.mac, 8);
}

static void build_unicast_header(struct MacFrameNormal *f, const uint8_t *dst_mac)
{
    f->fcs[0] = 0x41;
    f->fcs[1] = 0xCC;
    f->seq = radio_st.seq++;
    f->pan = PROTO_PAN_ID;
    memcpy(f->dst, dst_mac, 8);
    memcpy(f->src, radio_st.mac, 8);
}

// Wait up to ms for a received frame, sleeping the CPU between polls.
// Returns pointer to received frame data, or NULL on timeout.
static uint8_t *wait_for_rx(uint32_t ms, uint8_t *out_len, int8_t *out_rssi)
{
    uint32_t t0 = oepl_rf_rat_now();
    for (;;) {
        uint8_t *pkt = oepl_rf_rx_get(out_len, out_rssi);
        if (pkt) return pkt;
        if ((uint32_t)(oepl_rf_rat_now() - t0) >= ms * RF_RAT_TICKS_PER_MS) return NULL;
        oepl_hw_idle();
    }
}

// --- Public API ---

void oepl_radio_init(void)
{
    memset(&radio_st, 0, sizeof(radio_st));
    oepl_rf_get_mac(radio_st.mac);
    radio_st.ap_found = false;
}

int8_t oepl_radio_scan_channels(void)
{
    rtt_puts("Scan:");

#ifdef BENCH_SCAN_CH27_ONLY
    for (uint8_t ch = OEPL_NUM_CHANNELS - 1; ch < OEPL_NUM_CHANNELS; ch++) {
#else
    for (uint8_t ch = 0; ch < OEPL_NUM_CHANNELS; ch++) {
#endif
        rf_status_t rc = oepl_rf_set_channel(ch);
        if (rc != RF_OK) continue;

        uint8_t ieee_ch = oepl_channel_map[ch];
        rtt_puts(" ");
        rtt_put_hex8(ieee_ch);

        // Build PING frame
        struct MacFrameBcast *hdr = (struct MacFrameBcast *)tx_frame;
        build_bcast_header(hdr);
        tx_frame[sizeof(struct MacFrameBcast)] = PKT_PING;
        uint8_t tx_len = sizeof(struct MacFrameBcast) + 1;

        // Try up to 5 times per channel
        for (uint8_t attempt = 0; attempt < 5; attempt++) {
            // 1. Start RX first (background, 300ms timeout)
            rc = oepl_rf_rx_start(ieee_ch, 300000);
            if (rc != RF_OK) continue;

            // 2. TX PING (foreground within RX context)
            rc = oepl_rf_tx(tx_frame, tx_len);
            if (rc != RF_OK) {
                oepl_rf_rx_stop();
                continue;
            }

            // 3. Wait for PONG response — keep polling while RX is active
            for (uint8_t w = 0; w < 10; w++) {
                uint8_t pkt_len;
                int8_t rssi;
                uint8_t *pkt = wait_for_rx(50, &pkt_len, &rssi);
                if (pkt) {
                    // PONG: MacFrameNormal(21) + PKT_PONG(1) + channel(1)
                    if (pkt_len >= sizeof(struct MacFrameNormal) + 2) {
                        uint8_t pkt_type = pkt[sizeof(struct MacFrameNormal)];
                        if (pkt_type == PKT_PONG) {
                            struct MacFrameNormal *resp = (struct MacFrameNormal *)pkt;
                            memcpy(radio_st.ap_mac, resp->src, 8);
                            radio_st.current_channel = ch;
                            radio_st.current_ieee_ch = ieee_ch;
                            radio_st.last_rssi = rssi;
                            radio_st.ap_found = true;

                            oepl_rf_rx_stop();
                            oepl_rf_rx_flush();

                            rtt_puts(" PONG! RSSI=");
                            rtt_put_hex8((uint8_t)rssi);
                            rtt_puts("\r\n");
                            return (int8_t)ch;
                        }
                    }
                    oepl_rf_rx_flush();
                } else {
                    // No packet — check if RX is still active
                    if (oepl_rf_rx_ended()) break;
                }
            }

            // 4. Stop RX
            oepl_rf_rx_stop();
        }
    }

    rtt_puts(" none\r\n");
    return -1;
}

bool oepl_radio_checkin(struct AvailDataInfo *out_info)
{
    if (!radio_st.ap_found) return false;

    // Build AvailDataReq frame:
    // MacFrameBcast(17) + PKT_TYPE(1) + AvailDataReq(21) + pad(1) = 40 bytes
    // AP checks ret==40 exactly — the padding byte is required!
    struct MacFrameBcast *hdr = (struct MacFrameBcast *)tx_frame;
    build_bcast_header(hdr);
    tx_frame[sizeof(struct MacFrameBcast)] = PKT_AVAIL_DATA_REQ;

    struct AvailDataReq *req = (struct AvailDataReq *)&tx_frame[sizeof(struct MacFrameBcast) + 1];
    memset(req, 0, sizeof(struct AvailDataReq));
    req->lastPacketLQI = radio_st.last_lqi;
    req->lastPacketRSSI = radio_st.last_rssi;
    int8_t temp_c;
    uint16_t bat_mv;
    oepl_hw_get_temperature(&temp_c);
    oepl_hw_get_voltage(&bat_mv);
    req->temperature = temp_c;
    req->batteryMv = bat_mv;
    if (g_fault_pending) {
        // Crash report rides in the telemetry fields for this one checkin.
        // It stays pending until the AP actually answers: the check-in that
        // carries a crash report is the one most likely to fail (the tag has
        // just reset), and clearing it here threw the report away on a
        // check-in the AP never heard.
        req->batteryMv = g_fault_detail;
        req->temperature = (int8_t)g_fault_class;
        req->lastPacketLQI = g_fault_why;
        g_fault_reported = true;
    }
#ifdef DIAG_TELEMETRY
    else if (g_diag_pending) {
        req->lastPacketLQI = g_diag_lqi;
        req->temperature = (int8_t)g_diag_temp;
        req->batteryMv = g_diag_bat;
        g_diag_reported = true;
    }
#endif
    req->hwType = HW_TYPE;
    req->wakeupReason = g_wakeup_reason;
    req->capabilities = 0;
    req->tagSoftwareVersion = TAG_FW_VERSION;
    req->currentChannel = radio_st.current_channel;
    req->customMode = 0;
    add_crc(req, sizeof(struct AvailDataReq));

    // Padding byte after struct (AP expects exactly 40 bytes MPDU)
    tx_frame[sizeof(struct MacFrameBcast) + 1 + sizeof(struct AvailDataReq)] = 0x00;
    uint8_t tx_len = sizeof(struct MacFrameBcast) + 1 + sizeof(struct AvailDataReq) + 1;

    rtt_puts("TX ADR len=");
    rtt_put_hex8(tx_len);
    rtt_puts("\r\n");

    // 1. Start RX (background, 5s timeout) with explicit channel
    rf_status_t rc = oepl_rf_rx_start(radio_st.current_ieee_ch, 5000000);
    if (rc != RF_OK) return false;

    // 2. TX AvailDataReq (foreground within RX)
    rc = oepl_rf_tx(tx_frame, tx_len);
    if (rc != RF_OK) {
        oepl_rf_rx_stop();
        rtt_puts("TX fail\r\n");
        return false;
    }
    rtt_puts("TX OK\r\n");

    // 3. Wait for AvailDataInfo response — keep polling while RX is active
    for (uint8_t attempts = 0; attempts < 50; attempts++) {
        uint8_t pkt_len;
        int8_t rssi;
        uint8_t *pkt = wait_for_rx(100, &pkt_len, &rssi);
        if (!pkt) {
            // No packet yet — check if RX is still active
            if (oepl_rf_rx_ended()) {
                rtt_puts("RX: ended\r\n");
                break;
            }
            continue;  // RX still active, keep polling
        }

        // Dump first bytes of received frame
        rtt_puts("RX: len=");
        rtt_put_hex8(pkt_len);
        rtt_puts(" [");
        uint8_t dump_len = (pkt_len > 16) ? 16 : pkt_len;
        for (uint8_t i = 0; i < dump_len; i++) {
            rtt_put_hex8(pkt[i]);
            if (i < dump_len - 1) rtt_puts(" ");
        }
        rtt_puts("]\r\n");

        if (pkt_len >= sizeof(struct MacFrameNormal) + 1 + sizeof(struct AvailDataInfo)) {
            uint8_t pkt_type = pkt[sizeof(struct MacFrameNormal)];
            if (pkt_type == PKT_AVAIL_DATA_INFO) {
                struct AvailDataInfo *info = (struct AvailDataInfo *)&pkt[sizeof(struct MacFrameNormal) + 1];
                if (check_crc(info, sizeof(struct AvailDataInfo))) {
                    memcpy(out_info, info, sizeof(struct AvailDataInfo));
                    // Learn the AP's address from its reply. After a failed
                    // scan the direct check-in path has only a broadcast
                    // placeholder, and block requests / XferComplete are
                    // unicast to ap_mac.
                    memcpy(radio_st.ap_mac, ((struct MacFrameNormal *)pkt)->src, 8);
                    radio_st.last_rssi = rssi;
                    oepl_rf_rx_stop();
                    oepl_rf_rx_flush();
                    rtt_puts("Got AvailDataInfo type=");
                    rtt_put_hex8(info->dataType);
                    rtt_puts("\r\n");
                    // The AP answered, so anything that rode out with this
                    // check-in has been delivered.
                    if (g_fault_reported) {
                        g_fault_pending = false;
                        g_fault_reported = false;
                    }
#ifdef DIAG_TELEMETRY
                    if (g_diag_reported) {
                        g_diag_pending = false;
                        g_diag_reported = false;
                    }
#endif
                    return true;
                }
                rtt_puts("CRC fail\r\n");
            }
        }
        oepl_rf_rx_flush();
    }

    oepl_rf_rx_stop();
    rtt_puts("No AvailDataInfo\r\n");
    return false;
}

bool oepl_radio_send_xfer_complete(void)
{
    if (!radio_st.ap_found) return false;

    struct MacFrameNormal *hdr = (struct MacFrameNormal *)tx_frame;
    build_unicast_header(hdr, radio_st.ap_mac);
    tx_frame[sizeof(struct MacFrameNormal)] = PKT_XFER_COMPLETE;
    uint8_t tx_len = sizeof(struct MacFrameNormal) + 1;

    // The AP answers every XferComplete with XferCompleteAck (and only acts
    // on the first one after a checkin), so it's safe to repeat until acked.
#ifdef DIAG_TELEMETRY
    g_xfer_last_try = 0;
    g_xfer_tx_ok = false;
#endif
    for (uint8_t attempt = 0; attempt < XFER_COMPLETE_TRIES; attempt++) {
        if (attempt > 0) oepl_hw_delay_ms(100);
        hdr->seq = radio_st.seq++;

        rf_status_t rc = oepl_rf_rx_start(radio_st.current_ieee_ch,
                                          (XFER_COMPLETE_ACK_TIMEOUT_MS + 100) * 1000UL);
        if (rc != RF_OK) continue;

        rtt_puts("TX XferComplete");
        rc = oepl_rf_tx(tx_frame, tx_len);
        if (rc != RF_OK) {
            oepl_rf_rx_stop();
            rtt_puts(" TXfail\r\n");
            continue;
        }

#ifdef DIAG_TELEMETRY
        g_xfer_tx_ok = true;
#endif
        bool acked = false;
        uint32_t t0 = oepl_rf_rat_now();
        while ((uint32_t)(oepl_rf_rat_now() - t0) <
               XFER_COMPLETE_ACK_TIMEOUT_MS * RF_RAT_TICKS_PER_MS) {
            uint8_t pkt_len;
            int8_t rssi;
            uint8_t *pkt = oepl_rf_rx_get(&pkt_len, &rssi);
            if (!pkt) { oepl_hw_idle(); continue; }
            uint8_t hsz = mac_hdr_size(pkt, pkt_len);
            if (hsz > 0 && pkt_len > hsz && pkt[hsz] == PKT_XFER_COMPLETE_ACK) acked = true;
            oepl_rf_rx_flush();
            if (acked) break;
        }
        oepl_rf_rx_stop();
        oepl_rf_rx_flush_all();

        if (acked) {
            rtt_puts(" ACK\r\n");
#ifdef DIAG_TELEMETRY
            g_xfer_last_try = attempt + 1;
#endif
            return true;
        }
        rtt_puts(" noACK\r\n");
    }
    return false;
}

// Determine MAC header size from Frame Control field
static uint8_t mac_hdr_size(const uint8_t *pkt, uint8_t pkt_len)
{
    if (pkt_len < 3) return 0;
    uint8_t dst_mode = (pkt[1] >> 2) & 0x03;
    uint8_t src_mode = (pkt[1] >> 6) & 0x03;
    bool pan_compress = (pkt[0] >> 6) & 0x01;

    uint8_t sz = 3;  // FCS(2) + seq(1)
    if (dst_mode == 2) sz += 2 + 2;       // dst PAN + short addr
    else if (dst_mode == 3) sz += 2 + 8;  // dst PAN + extended addr
    if (src_mode == 2) sz += (pan_compress ? 0 : 2) + 2;
    else if (src_mode == 3) sz += (pan_compress ? 0 : 2) + 8;
    return sz;
}

uint8_t oepl_radio_request_block(uint8_t block_id, uint64_t data_ver, uint8_t data_type,
                                  uint8_t *block_buf, uint8_t *parts_rcvd)
{
    if (!radio_st.ap_found) return 0;

    // Count parts already received (from prior attempts)
    uint8_t total_parts = 0;
    for (uint8_t i = 0; i < BLOCK_MAX_PARTS; i++) {
        if (parts_rcvd[i / 8] & (1 << (i % 8))) total_parts++;
    }
    if (total_parts >= BLOCK_MAX_PARTS) return total_parts;

    // A first request makes the AP fetch the block from its host (pleaseWaitMs
    // ~550ms on a C6 AP). Once we hold part of a block, ask for the rest with
    // a PARTIAL request, which the AP serves straight from its buffer.
    struct MacFrameNormal *hdr = (struct MacFrameNormal *)tx_frame;
    build_unicast_header(hdr, radio_st.ap_mac);
    tx_frame[sizeof(struct MacFrameNormal)] =
        (total_parts > 0) ? PKT_BLOCK_PARTIAL_REQUEST : PKT_BLOCK_REQUEST;

    struct BlockRequest *breq = (struct BlockRequest *)&tx_frame[sizeof(struct MacFrameNormal) + 1];
    memset(breq, 0, sizeof(struct BlockRequest));
    breq->ver = data_ver;
    breq->blockId = block_id;
    breq->type = data_type;
    // Request only missing parts (complement of parts_rcvd)
    for (uint8_t i = 0; i < BLOCK_REQ_PARTS_BYTES; i++)
        breq->requestedParts[i] = ~parts_rcvd[i];
    breq->requestedParts[5] &= 0x03;  // Only bits 0-41 valid
    add_crc(breq, sizeof(struct BlockRequest));

    uint8_t tx_len = sizeof(struct MacFrameNormal) + 1 + sizeof(struct BlockRequest);

    rtt_puts("BRQ b=");
    rtt_put_hex8(block_id);

    // Single long RX session: covers ack + wait + all parts
    rf_status_t rc = oepl_rf_rx_start(radio_st.current_ieee_ch, BLOCK_RX_WINDOW_MS * 1000UL);
    if (rc != RF_OK) { rtt_puts(" RXfail\r\n"); return total_parts; }

    // TX block request
    rc = oepl_rf_tx(tx_frame, tx_len);
    if (rc != RF_OK) {
        oepl_rf_rx_stop();
        rtt_puts(" TXfail\r\n");
        return total_parts;
    }
    rtt_puts(" TX+\r\n");

    // Receive ack + parts in one continuous RX session.
    //
    // The AP answers with an ACK (pleaseWaitMs), then sends the requested
    // parts as a burst. Rather than sit out the whole RX window when a part
    // is lost, stop once the burst has gone quiet and let the caller
    // re-request the missing parts. Before any part has arrived, allow for
    // the AP's pleaseWaitMs (it may be rendering the image); after parts
    // start flowing, BLOCK_PART_TIMEOUT_MS of silence means the burst is
    // over. Timeouts are generous because AP pacing varies widely (see
    // oepl_radio_cc2630.h); the BP: line reports the measured gap.
    bool got_ack = false;
    bool got_cancel = false;
    uint8_t other_pkts = 0;
    uint8_t parts_this_req = 0;
    uint32_t t_last = oepl_rf_rat_now();
    uint32_t t_first_part = 0, t_last_part = 0;   // for pacing measurement
    uint32_t idle_limit = BLOCK_ACK_TIMEOUT_MS * RF_RAT_TICKS_PER_MS;
    bool draining = false;          // block complete, waiting for the AP's burst to end
    uint32_t t_drain_start = 0;

    // Bounded by the RX window / idle timeout, with a wall-clock backstop
    uint32_t t_req = oepl_rf_rat_now();
    for (;;) {
        if ((uint32_t)(oepl_rf_rat_now() - t_req) >
            (BLOCK_RX_WINDOW_MS + 2000) * RF_RAT_TICKS_PER_MS) break;
        uint8_t pkt_len;
        int8_t rssi;
        uint8_t *pkt = oepl_rf_rx_get(&pkt_len, &rssi);
        if (draining && (uint32_t)(oepl_rf_rat_now() - t_drain_start) >
                        BLOCK_DRAIN_MAX_MS * RF_RAT_TICKS_PER_MS) {
            if (pkt) oepl_rf_rx_flush();
            break;
        }
        if (!pkt) {
            if ((uint32_t)(oepl_rf_rat_now() - t_last) > idle_limit) break;
            if (oepl_rf_rx_ended()) break;
            oepl_hw_idle();
            continue;
        }
        // Only frames that are part of *our* transfer extend the wait. Other
        // traffic (frames the address filter rejected still land in the
        // queue, e.g. the AP serving another tag) must not keep us listening.
        uint32_t t_now = oepl_rf_rat_now();

        // Parse header size from frame control
        uint8_t hsz = mac_hdr_size(pkt, pkt_len);
        if (hsz > 0 && pkt_len > hsz) {
            uint8_t pkt_type = pkt[hsz];

            if (pkt_type == PKT_CANCEL_XFER) {
                // AP is busy with another tag (or has nothing for us): stop
                // listening now; the caller retries after a pause.
                got_cancel = true;
                oepl_rf_rx_flush();
                break;
            } else if (pkt_type == PKT_BLOCK_REQUEST_ACK && pkt_len >= hsz + 1 + 3) {
                struct BlockRequestAck *ack = (struct BlockRequestAck *)&pkt[hsz + 1];
                got_ack = true;
                t_last = t_now;
                rtt_puts("ACK w=");
                rtt_put_hex8((ack->pleaseWaitMs >> 8) & 0xFF);
                rtt_put_hex8(ack->pleaseWaitMs & 0xFF);
                rtt_puts("\r\n");
                // Parts follow after pleaseWaitMs; allow that plus a margin
                idle_limit = ((uint32_t)ack->pleaseWaitMs + BLOCK_PART_TIMEOUT_MS)
                             * RF_RAT_TICKS_PER_MS;
            } else if (pkt_type == PKT_BLOCK_PART &&
                       pkt_len >= hsz + 1 + sizeof(struct BlockPart)) {
                struct BlockPart *bp = (struct BlockPart *)&pkt[hsz + 1];
                if (bp->blockId == block_id && bp->blockPart < BLOCK_MAX_PARTS) {
                    t_last = t_now;
                    uint16_t offset = (uint16_t)bp->blockPart * BLOCK_PART_DATA_SIZE;
                    uint16_t copy_len = BLOCK_PART_DATA_SIZE;
                    if (offset + copy_len > BLOCK_XFER_BUFFER_SIZE)
                        copy_len = BLOCK_XFER_BUFFER_SIZE - offset;
                    memcpy(&block_buf[offset], bp->data, copy_len);

                    uint8_t byte_idx = bp->blockPart / 8;
                    uint8_t bit_idx = bp->blockPart % 8;
                    if (!(parts_rcvd[byte_idx] & (1 << bit_idx))) {
                        parts_rcvd[byte_idx] |= (1 << bit_idx);
                        total_parts++;
                    }
                    if (parts_this_req == 0) t_first_part = t_last;
                    t_last_part = t_last;
                    parts_this_req++;
                    // Burst is flowing: silence now means it's over
                    idle_limit = BLOCK_PART_TIMEOUT_MS * RF_RAT_TICKS_PER_MS;
                }
            } else {
                other_pkts++;
            }
        } else {
            other_pkts++;
        }

        oepl_rf_rx_flush();
        if (total_parts >= BLOCK_MAX_PARTS && !draining) {
            draining = true;
            t_drain_start = oepl_rf_rat_now();
            idle_limit = BLOCK_DRAIN_QUIET_MS * RF_RAT_TICKS_PER_MS;
        }
    }
    oepl_rf_rx_stop();

    rtt_puts("BP:");
    rtt_put_hex8(total_parts);
    rtt_puts("/");
    rtt_put_hex8(BLOCK_MAX_PARTS);
    rtt_puts(" new=");
    rtt_put_hex8(parts_this_req);
    if (parts_this_req > 1) {
        // Average gap between parts this request, in ms (hex)
        uint32_t gap_ms = (t_last_part - t_first_part) / RF_RAT_TICKS_PER_MS / (parts_this_req - 1);
        rtt_puts(" gap=");
        rtt_put_hex8((gap_ms >> 8) & 0xFF);
        rtt_put_hex8(gap_ms & 0xFF);
    }
    rf_rx_stats_t st;
    oepl_rf_rx_stats(&st);
#ifdef DIAG_TELEMETRY
    g_diag_requests++;
    g_diag_rf_nok += st.nok;
    g_diag_rf_full += st.buf_full;
#endif
    rtt_puts(" rf[d=");   rtt_put_hex8(st.data);
    rtt_puts(" nok=");    rtt_put_hex8(st.nok);
    rtt_puts(" ign=");    rtt_put_hex8(st.ignored);
    rtt_puts(" full=");   rtt_put_hex8(st.buf_full);
    rtt_puts("]");
    if (!got_ack) rtt_puts(" noACK");
    if (got_cancel) rtt_puts(" CANCEL");
    if (other_pkts) { rtt_puts(" oth="); rtt_put_hex8(other_pkts); }
    rtt_puts("\r\n");

    // AP told us to back off: give the other transfer its 1.2s exclusivity
    // window (CONCURRENT_REQUEST_DELAY on the AP) before the caller retries.
    if (got_cancel) oepl_hw_delay_ms(1500);

    return total_parts;
}

radio_state_t *oepl_radio_get_state(void)
{
    return &radio_st;
}

void oepl_radio_set_wakeup_reason(uint8_t reason)
{
    g_wakeup_reason = reason;
}

#ifdef DIAG_TELEMETRY
void oepl_radio_set_diag_report(uint8_t failed_blocks, uint8_t xfer,
                                uint8_t requests, uint8_t rf_nok, uint8_t rf_full)
{
    g_diag_lqi = (uint8_t)((failed_blocks & 0xF) << 4) | (xfer & 0xF);
    g_diag_temp = requests > 127 ? 127 : requests;
    g_diag_bat = (uint16_t)rf_nok << 8 | rf_full;
    g_diag_pending = true;
}
#endif

void oepl_radio_set_fault_report(uint8_t fault_class, uint16_t detail, uint8_t why)
{
    g_fault_class = fault_class;
    g_fault_detail = detail;
    g_fault_why = why;
    g_fault_pending = true;
}
