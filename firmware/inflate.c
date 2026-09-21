// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nathan Bigelow
// -----------------------------------------------------------------------------
//  Minimal DEFLATE/zlib decompressor — see inflate.h for why it is shaped this
//  way. Decoding is the straightforward canonical-Huffman walk (one bit at a
//  time, no lookup tables) because the tables would cost more RAM than the
//  whole decoder is allowed and the tag has time to spare: a full 600x448 BWR
//  image decodes in well under a second on this part.
// -----------------------------------------------------------------------------

#include "inflate.h"

// RFC 1951 section 3.2.5
static const uint16_t len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const uint8_t dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
// Order in which the code-length code lengths appear (RFC 1951 3.2.7)
static const uint8_t clen_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

// --- input ---

static int need_bits(struct inflate_ctx *c, uint8_t n)
{
    while (c->bitcnt < n) {
        if (c->in_pos >= c->in_len) return INFLATE_ERR_INPUT;
        c->bitbuf |= (uint32_t)c->in[c->in_pos++] << c->bitcnt;
        c->bitcnt += 8;
    }
    return INFLATE_OK;
}

// Read n bits (n <= 24), LSB first. Returns negative on running out of input.
static int32_t get_bits(struct inflate_ctx *c, uint8_t n)
{
    if (n == 0) return 0;
    int rc = need_bits(c, n);
    if (rc != INFLATE_OK) return rc;
    uint32_t v = c->bitbuf & ((1UL << n) - 1);
    c->bitbuf >>= n;
    c->bitcnt -= n;
    return (int32_t)v;
}

// --- output ---

static int flush_window(struct inflate_ctx *c, uint32_t len)
{
    if (len == 0) return INFLATE_OK;
    if (!c->sink(c->sink_ctx, c->win, len)) return INFLATE_ERR_SINK;
    return INFLATE_OK;
}

static int emit(struct inflate_ctx *c, uint8_t b)
{
    if (c->out_limit && c->out_total >= c->out_limit) return INFLATE_ERR_LIMIT;
    c->win[c->win_pos++] = b;
    c->out_total++;
    // Adler-32 (RFC 1950): a = 1 + sum(bytes), b = sum(a), both mod 65521
    c->adler_a += b;
    if (c->adler_a >= 65521) c->adler_a -= 65521;
    c->adler_b += c->adler_a;
    if (c->adler_b >= 65521) c->adler_b -= 65521;
    if (c->win_pos == c->win_size) {
        int rc = flush_window(c, c->win_size);
        if (rc != INFLATE_OK) return rc;
        c->win_pos = 0;
    }
    return INFLATE_OK;
}

// --- Huffman ---

// Build a canonical code from a list of code lengths.
static int huff_build(struct inflate_huff *h, const uint8_t *lengths, uint16_t n)
{
    uint16_t offsets[16];
    for (uint8_t i = 0; i < 16; i++) h->counts[i] = 0;
    for (uint16_t i = 0; i < n; i++) h->counts[lengths[i]]++;
    // An empty tree is legal (a block with no matches has no distance codes);
    // decoding from it is what fails, not building it.
    if (h->counts[0] == n) return INFLATE_OK;

    // over-subscribed or incomplete sets are malformed (an incomplete set with
    // a single code is legal for the distance tree, so allow that one case)
    int left = 1;
    for (uint8_t len = 1; len < 16; len++) {
        left <<= 1;
        left -= h->counts[len];
        if (left < 0) return INFLATE_ERR_HUFFMAN;
    }

    offsets[0] = offsets[1] = 0;
    for (uint8_t len = 1; len < 15; len++)
        offsets[len + 1] = offsets[len] + h->counts[len];
    for (uint16_t i = 0; i < n; i++)
        if (lengths[i]) h->symbols[offsets[lengths[i]]++] = i;
    return INFLATE_OK;
}

// Decode one symbol, walking code lengths shortest-first.
static int32_t huff_decode(struct inflate_ctx *c, const struct inflate_huff *h)
{
    int32_t code = 0, first = 0, index = 0;
    for (uint8_t len = 1; len < 16; len++) {
        int32_t bit = get_bits(c, 1);
        if (bit < 0) return bit;
        code |= bit;
        int32_t count = h->counts[len];
        if (code - first < count) return h->symbols[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return INFLATE_ERR_HUFFMAN;
}

static void huff_fixed(struct inflate_ctx *c)
{
    uint8_t lengths[288];
    uint16_t i = 0;
    for (; i < 144; i++) lengths[i] = 8;
    for (; i < 256; i++) lengths[i] = 9;
    for (; i < 280; i++) lengths[i] = 7;
    for (; i < 288; i++) lengths[i] = 8;
    huff_build(&c->lit, lengths, 288);
    for (i = 0; i < 30; i++) lengths[i] = 5;
    huff_build(&c->dist, lengths, 30);
}

static int huff_dynamic(struct inflate_ctx *c)
{
    int32_t hlit = get_bits(c, 5);
    int32_t hdist = get_bits(c, 5);
    int32_t hclen = get_bits(c, 4);
    if (hlit < 0 || hdist < 0 || hclen < 0) return INFLATE_ERR_INPUT;
    uint16_t nlit = (uint16_t)hlit + 257;
    uint16_t ndist = (uint16_t)hdist + 1;
    uint16_t nclen = (uint16_t)hclen + 4;
    if (nlit > 286 || ndist > 30) return INFLATE_ERR_HUFFMAN;

    uint8_t lengths[288 + 32];
    for (uint8_t i = 0; i < 19; i++) lengths[i] = 0;
    for (uint16_t i = 0; i < nclen; i++) {
        int32_t v = get_bits(c, 3);
        if (v < 0) return INFLATE_ERR_INPUT;
        lengths[clen_order[i]] = (uint8_t)v;
    }
    // the code-length code goes into lit temporarily
    int rc = huff_build(&c->lit, lengths, 19);
    if (rc != INFLATE_OK) return rc;

    uint16_t n = 0;
    while (n < nlit + ndist) {
        int32_t sym = huff_decode(c, &c->lit);
        if (sym < 0) return sym;
        if (sym < 16) {
            lengths[n++] = (uint8_t)sym;
        } else {
            uint8_t rep_len = 0, value = 0;
            int32_t extra;
            if (sym == 16) {
                if (n == 0) return INFLATE_ERR_HUFFMAN;
                value = lengths[n - 1];
                extra = get_bits(c, 2);
                if (extra < 0) return INFLATE_ERR_INPUT;
                rep_len = 3 + (uint8_t)extra;
            } else if (sym == 17) {
                extra = get_bits(c, 3);
                if (extra < 0) return INFLATE_ERR_INPUT;
                rep_len = 3 + (uint8_t)extra;
            } else if (sym == 18) {
                extra = get_bits(c, 7);
                if (extra < 0) return INFLATE_ERR_INPUT;
                rep_len = 11 + (uint8_t)extra;
            } else {
                return INFLATE_ERR_HUFFMAN;
            }
            if (n + rep_len > nlit + ndist) return INFLATE_ERR_HUFFMAN;
            while (rep_len--) lengths[n++] = value;
        }
    }
    if (lengths[256] == 0) return INFLATE_ERR_HUFFMAN;   // no end-of-block code

    rc = huff_build(&c->lit, lengths, nlit);
    if (rc != INFLATE_OK) return rc;
    return huff_build(&c->dist, lengths + nlit, ndist);
}

// --- blocks ---

static int block_stored(struct inflate_ctx *c)
{
    // Stored blocks start on a byte boundary. need_bits() reads ahead, so give
    // back the whole bytes still sitting in the bit buffer before dropping the
    // partial byte.
    c->in_pos -= (c->bitcnt >> 3);
    c->bitbuf = 0;
    c->bitcnt = 0;
    if (c->in_pos + 4 > c->in_len) return INFLATE_ERR_INPUT;
    uint16_t len = (uint16_t)(c->in[c->in_pos] | (c->in[c->in_pos + 1] << 8));
    uint16_t nlen = (uint16_t)(c->in[c->in_pos + 2] | (c->in[c->in_pos + 3] << 8));
    c->in_pos += 4;
    if ((uint16_t)~len != nlen) return INFLATE_ERR_FORMAT;
    if (c->in_pos + len > c->in_len) return INFLATE_ERR_INPUT;
    while (len--) {
        int rc = emit(c, c->in[c->in_pos++]);
        if (rc != INFLATE_OK) return rc;
    }
    return INFLATE_OK;
}

static int block_huffman(struct inflate_ctx *c)
{
    for (;;) {
        int32_t sym = huff_decode(c, &c->lit);
        if (sym < 0) return sym;
        if (sym < 256) {
            int rc = emit(c, (uint8_t)sym);
            if (rc != INFLATE_OK) return rc;
            continue;
        }
        if (sym == 256) return INFLATE_OK;        // end of block
        sym -= 257;
        if (sym >= 29) return INFLATE_ERR_HUFFMAN;
        int32_t extra = get_bits(c, len_extra[sym]);
        if (extra < 0) return INFLATE_ERR_INPUT;
        uint32_t length = len_base[sym] + (uint32_t)extra;

        int32_t dsym = huff_decode(c, &c->dist);
        if (dsym < 0) return dsym;
        if (dsym >= 30) return INFLATE_ERR_HUFFMAN;
        extra = get_bits(c, dist_extra[dsym]);
        if (extra < 0) return INFLATE_ERR_INPUT;
        uint32_t dist = dist_base[dsym] + (uint32_t)extra;

        if (dist > c->out_total || dist > c->win_size) return INFLATE_ERR_DISTANCE;

        uint32_t src = (c->win_pos - dist) & (c->win_size - 1);
        while (length--) {
            uint8_t b = c->win[src];
            src = (src + 1) & (c->win_size - 1);
            int rc = emit(c, b);
            if (rc != INFLATE_OK) return rc;
        }
    }
}

// --- entry point ---

int inflate_zlib(const uint8_t *in, uint32_t in_len,
                 uint8_t *win, uint32_t win_size,
                 inflate_sink_fn sink, void *sink_ctx,
                 uint32_t out_limit, uint32_t *out_bytes)
{
    // Static, not on the stack: the context is ~750 bytes and the tag's stack
    // is the only thing between .noinit and the top of SRAM.
    static struct inflate_ctx c;
    if (out_bytes) *out_bytes = 0;
    if (win_size < 256 || (win_size & (win_size - 1))) return INFLATE_ERR_WINDOW;
    if (in_len < 2 + 4) return INFLATE_ERR_INPUT;

    c.in = in; c.in_len = in_len; c.in_pos = 0;
    c.bitbuf = 0; c.bitcnt = 0;
    c.win = win; c.win_size = win_size; c.win_pos = 0;
    c.sink = sink; c.sink_ctx = sink_ctx;
    c.out_total = 0; c.out_limit = out_limit;
    c.adler_a = 1; c.adler_b = 0;

    // RFC 1950 header
    uint8_t cmf = in[0], flg = in[1];
    if ((cmf & 0x0F) != 8) return INFLATE_ERR_FORMAT;           // not deflate
    if (((cmf << 8) | flg) % 31) return INFLATE_ERR_FORMAT;     // bad check
    if (flg & 0x20) return INFLATE_ERR_FORMAT;                  // preset dict
    uint32_t stream_window = 1UL << (((cmf >> 4) & 0x0F) + 8);
    if (stream_window > win_size) return INFLATE_ERR_WINDOW;
    c.in_pos = 2;

    int rc;
    for (;;) {
        int32_t final = get_bits(&c, 1);
        int32_t type = get_bits(&c, 2);
        if (final < 0 || type < 0) { rc = INFLATE_ERR_INPUT; goto done; }
        if (type == 0) {
            rc = block_stored(&c);
        } else if (type == 1) {
            huff_fixed(&c);
            rc = block_huffman(&c);
        } else if (type == 2) {
            rc = huff_dynamic(&c);
            if (rc == INFLATE_OK) rc = block_huffman(&c);
        } else {
            rc = INFLATE_ERR_FORMAT;
        }
        if (rc != INFLATE_OK) goto done;
        if (final) break;
    }

    rc = flush_window(&c, c.win_pos);
    if (rc != INFLATE_OK) goto done;
    c.win_pos = 0;

    // Adler-32 trailer, byte-aligned and big-endian
    {
        uint32_t trailer_at = c.in_pos - (c.bitcnt >> 3);
        if (trailer_at + 4 > c.in_len) { rc = INFLATE_ERR_INPUT; goto done; }
        uint32_t want = ((uint32_t)in[trailer_at] << 24) | ((uint32_t)in[trailer_at + 1] << 16) |
                        ((uint32_t)in[trailer_at + 2] << 8) | in[trailer_at + 3];
        uint32_t got = (c.adler_b << 16) | c.adler_a;
        if (want != got) { rc = INFLATE_ERR_ADLER; goto done; }
    }
    rc = INFLATE_OK;

done:
    // On failure the caller throws the output away, so nothing more is emitted.
    if (out_bytes) *out_bytes = c.out_total;
    return rc;
}
