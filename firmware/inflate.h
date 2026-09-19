// -----------------------------------------------------------------------------
//  Minimal DEFLATE/zlib decompressor for the CC2630 tag
//
//  Sized for this tag rather than for generality:
//
//  - The input is addressable in one piece (flash or RAM), so the decoder never
//    has to suspend and resume; images are spooled to staging flash first.
//  - The window is caller-supplied and may be as small as the stream needs.
//    The AP compresses with a 4 KB dictionary (miniz-oepl: TDEFL_LZ_DICT_SIZE
//    4096, and it stamps CINFO=4 into the zlib header), so 4 KB is enough for
//    anything the AP sends; a stream that asks for more is rejected rather
//    than decoded wrongly.
//  - Output leaves through a sink callback in window-sized runs, so nothing
//    ever holds a whole image: the caller streams it to the panel or to flash.
//
//  State is ~800 bytes plus the window.
// -----------------------------------------------------------------------------

#ifndef INFLATE_H
#define INFLATE_H

#include <stdint.h>
#include <stdbool.h>

// Called with each run of decompressed bytes, in order. `len` is at most the
// window size. Return false to abort the decode (the sink hit its own error).
typedef bool (*inflate_sink_fn)(void *ctx, const uint8_t *data, uint32_t len);

struct inflate_huff {
    uint16_t counts[16];     // number of codes of each length
    uint16_t symbols[288];   // symbols ordered by code
};

struct inflate_ctx {
    // input (whole stream, addressable)
    const uint8_t *in;
    uint32_t in_len;
    uint32_t in_pos;
    uint32_t bitbuf;
    uint8_t  bitcnt;

    // circular output window, caller-supplied; size must be a power of two
    uint8_t *win;
    uint32_t win_size;
    uint32_t win_pos;

    // output
    inflate_sink_fn sink;
    void *sink_ctx;
    uint32_t out_total;      // bytes produced
    uint32_t out_limit;      // stop after this many (0 = no limit)

    // rolling Adler-32 of the output, checked against the stream's trailer
    uint32_t adler_a, adler_b;

    struct inflate_huff lit;
    struct inflate_huff dist;
};

#define INFLATE_OK             0
#define INFLATE_ERR_INPUT     -1   // ran out of input
#define INFLATE_ERR_FORMAT    -2   // not a zlib stream / bad block type
#define INFLATE_ERR_WINDOW    -3   // stream needs a bigger window than supplied
#define INFLATE_ERR_HUFFMAN   -4   // malformed Huffman data
#define INFLATE_ERR_DISTANCE  -5   // back-reference before the start of output
#define INFLATE_ERR_SINK      -6   // sink asked to stop
#define INFLATE_ERR_ADLER     -7   // output checksum mismatch
#define INFLATE_ERR_LIMIT     -8   // more output than out_limit

// Decompress a zlib stream (RFC 1950) from `in`. `win` is win_size bytes and
// win_size must be a power of two, >= 256. Returns INFLATE_OK or a negative
// error; *out_bytes (if given) gets the number of bytes produced either way.
int inflate_zlib(const uint8_t *in, uint32_t in_len,
                 uint8_t *win, uint32_t win_size,
                 inflate_sink_fn sink, void *sink_ctx,
                 uint32_t out_limit, uint32_t *out_bytes);

#endif // INFLATE_H
