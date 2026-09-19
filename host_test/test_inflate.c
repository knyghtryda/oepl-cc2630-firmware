// Host test for firmware/inflate.c: decode what the AP's own compressor
// produces and compare byte for byte against the raw image.
//   test_inflate <zlib_file> <expected_raw_file>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "inflate.h"

struct check {
    const uint8_t *want;
    uint32_t want_len;
    uint32_t pos;
    int mismatch;
    uint32_t runs;
};

static bool sink(void *ctx, const uint8_t *data, uint32_t len)
{
    struct check *k = ctx;
    k->runs++;
    for (uint32_t i = 0; i < len; i++) {
        if (k->pos < k->want_len && data[i] != k->want[k->pos] && !k->mismatch) {
            printf("  mismatch at output byte %u: got %02x want %02x\n",
                   k->pos, data[i], k->want[k->pos]);
            k->mismatch = 1;
        }
        k->pos++;
    }
    return true;
}

static uint8_t *slurp(const char *path, uint32_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n);
    if (fread(b, 1, n, f) != (size_t)n) exit(1);
    fclose(f);
    *len = (uint32_t)n;
    return b;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: test_inflate zlib raw\n"); return 2; }
    uint32_t zlen, rlen;
    uint8_t *z = slurp(argv[1], &zlen);
    uint8_t *raw = slurp(argv[2], &rlen);

    // the AP's container: uint32 uncompressed size, then the zlib stream
    uint32_t totalbytes;
    memcpy(&totalbytes, z, 4);

    // ... whose first 6 bytes are the image header, then the planes
    uint8_t expected[6 + 67200 * 2];
    uint32_t explen = 6 + rlen;
    expected[0] = 6;
    expected[1] = 600 & 0xFF; expected[2] = 600 >> 8;
    expected[3] = 448 & 0xFF; expected[4] = 448 >> 8;
    expected[5] = (uint8_t)(rlen == 67200 ? 2 : 1);
    memcpy(expected + 6, raw, rlen);

    struct check k = { .want = expected, .want_len = explen };
    static uint8_t win[4096];
    uint32_t produced = 0;
    int rc = inflate_zlib(z + 4, zlen - 4, win, sizeof(win), sink, &k,
                          totalbytes, &produced);

    printf("%s: rc=%d produced=%u expected=%u (totalbytes=%u) runs=%u\n",
           argv[1], rc, produced, explen, totalbytes, k.runs);
    if (rc != INFLATE_OK) { printf("  FAIL: decoder error %d\n", rc); return 1; }
    if (produced != explen) { printf("  FAIL: length\n"); return 1; }
    if (k.mismatch) { printf("  FAIL: content\n"); return 1; }
    printf("  OK: byte-exact\n");
    return 0;
}
