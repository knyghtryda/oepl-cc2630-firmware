// Robustness check: the tag runs this decoder on radio data, so a corrupt or
// truncated stream must return an error, not hang, overrun the sink, or read
// outside the input buffer.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "inflate.h"

static uint32_t produced_total, limit_hit;

static bool sink(void *ctx, const uint8_t *data, uint32_t len)
{
    (void)ctx; (void)data;
    produced_total += len;
    if (produced_total > 200000) { limit_hit++; return false; }   // runaway guard
    return true;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: fuzz_inflate zlib\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *orig = malloc(n);
    if (fread(orig, 1, n, f) != (size_t)n) return 1;
    fclose(f);

    uint32_t totalbytes;
    memcpy(&totalbytes, orig, 4);
    static uint8_t win[4096];
    uint8_t *buf = malloc(n);
    int errors[16] = {0}, ok = 0;

    srand(1234);
    // truncations
    for (long cut = 5; cut < n; cut += (n / 200) + 1) {
        memcpy(buf, orig, n);
        uint32_t got = 0;
        produced_total = 0;
        int rc = inflate_zlib(buf + 4, (uint32_t)(cut - 4), win, sizeof(win),
                              sink, NULL, totalbytes, &got);
        if (rc == INFLATE_OK) ok++; else errors[-rc & 15]++;
    }
    // single-byte corruptions
    for (int i = 0; i < 20000; i++) {
        memcpy(buf, orig, n);
        long at = 4 + rand() % (n - 4);
        buf[at] ^= (uint8_t)(1 << (rand() % 8));
        uint32_t got = 0;
        produced_total = 0;
        int rc = inflate_zlib(buf + 4, (uint32_t)(n - 4), win, sizeof(win),
                              sink, NULL, totalbytes, &got);
        if (rc == INFLATE_OK) ok++; else errors[-rc & 15]++;
    }
    free(orig); free(buf);
    printf("fuzz: %d accepted, errors by code:", ok);
    for (int i = 1; i < 9; i++) if (errors[i]) printf(" -%d:%d", i, errors[i]);
    printf("  runaway_aborts=%u\n", limit_hit);
    return 0;
}
