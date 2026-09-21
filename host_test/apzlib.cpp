// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nathan Bigelow
// Host replica of the AP's image compressor (ESP32_AP-Flasher/src/makeimage.cpp),
// so the tag-side decoder can be tested offline against bit-identical input.
//   apzlib <raw_in> <planes> <width> <height> <out>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "miniz-oepl.h"
using namespace Miniz;

int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "usage: apzlib raw planes w h out\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END); long rawsz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> raw(rawsz);
    if (fread(raw.data(), 1, rawsz, f) != (size_t)rawsz) return 1;
    fclose(f);

    int planes = atoi(argv[2]);
    uint16_t w = (uint16_t)atoi(argv[3]), h = (uint16_t)atoi(argv[4]);
    size_t plane_size = rawsz / planes;

    uint8_t hdr[6];
    hdr[0] = 6;
    memcpy(hdr + 1, &w, 2);
    memcpy(hdr + 3, &h, 2);
    hdr[5] = (uint8_t)planes;
    uint32_t totalbytes = (uint32_t)(plane_size * planes + 6);

    tdefl_compressor *comp = (tdefl_compressor *)malloc(sizeof(tdefl_compressor));
    // 1500 = the AP's "unofficial level 10", plus the zlib header
    if (tdefl_initOEPL(comp, NULL, NULL, TDEFL_WRITE_ZLIB_HEADER | 1500) != TDEFL_STATUS_OKAY)
        return 1;

    std::vector<uint8_t> out(rawsz * 2 + 1024);
    size_t outpos = 0;
    auto run = [&](const uint8_t *in, size_t inbytes, tdefl_flush flush) {
        size_t ib = inbytes, ob = out.size() - outpos;
        tdefl_compressOEPL(comp, in, &ib, out.data() + outpos, &ob, flush);
        outpos += ob;
    };
    run(hdr, sizeof(hdr), TDEFL_NO_FLUSH);
    run(raw.data(), plane_size, planes == 2 ? TDEFL_SYNC_FLUSH : TDEFL_FINISH);
    if (planes == 2) run(raw.data() + plane_size, plane_size, TDEFL_FINISH);

    // the AP rewrites the zlib header to advertise the 4 KB window it used
    const uint8_t cmf = 0x48;
    uint16_t header = cmf << 8 | (3 << 6);
    header += 31 - (header % 31);
    out[0] = cmf; out[1] = header & 0xFF;

    FILE *o = fopen(argv[5], "wb");
    fwrite(&totalbytes, 4, 1, o);          // uncompressed size, little-endian
    fwrite(out.data(), 1, outpos, o);
    fclose(o);
    fprintf(stderr, "%ld raw -> %zu compressed (+4 header), totalbytes=%u\n", rawsz, outpos, totalbytes);
    return 0;
}
