// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nathan Bigelow
// -----------------------------------------------------------------------------
//  NFC tag chip (NXP NTAG I2C family) on the TG-GR6000N
//
//  The board carries a passive NFC Type-2 tag IC with its own coil, separate
//  from the 2.4 GHz antenna. It has no transmitter: a phone powers it from
//  its own field, so it answers even when the tag's battery is flat or its
//  firmware is broken. The CC2630 reaches it over I2C:
//
//      DIO24 = SDA, DIO25 = SCL, slave address 0x55, 16-byte blocks
//      DIO21 = FD (field detect), open-drain, pulses LOW while a reader's
//              field is present — measured on the bench, 2026-09-19
//      DIO5  = shared peripheral power (panel *and* NFC), high = on
//
//  Block 0 holds the UID, the capability container and the chip's own I2C
//  address, and is never written here. User memory (the NDEF area) starts at
//  block 1, which is where both the stock firmware and the OEPL AP expect
//  content to go.
// -----------------------------------------------------------------------------

#ifndef OEPL_NFC_CC2630_H
#define OEPL_NFC_CC2630_H

#include <stdint.h>
#include <stdbool.h>

#define NFC_I2C_ADDR        0x55
#define NFC_BLOCK_SIZE      16
#define NFC_FIRST_USER_BLK  1
#define NFC_PIN_FD          21

// Bring up the I2C peripheral and check that the chip answers. Returns false
// if it is not populated on this board (the footprint is a per-variant
// option), in which case nothing else here should be called.
bool oepl_nfc_init(void);

// True once oepl_nfc_init() has found a chip.
bool oepl_nfc_present(void);

// Write `len` bytes into user memory starting at block 1, padding the last
// block with zeroes. Content is expected to be an NDEF TLV, which is what
// the AP sends as DATATYPE_NFC_RAW_CONTENT.
bool oepl_nfc_write_user(const uint8_t *data, uint16_t len);

// Read one 16-byte block (0 = the header block with the UID and CC).
bool oepl_nfc_read_block(uint8_t blk, uint8_t *out);

// Compose an NDEF text record into `out` (a TLV, ready for the chip) and
// return its length, or 0 if it does not fit.
uint16_t oepl_nfc_make_text(uint8_t *out, uint16_t out_size, const char *text,
                            uint8_t text_len);

#endif // OEPL_NFC_CC2630_H
