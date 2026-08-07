#pragma once

// framebarcode.h — decode the stream harness's frame-index barcode.
//
// The retro-stream stability harness paints a 24-cell strip across the top
// 1/16 of every test frame: 4 sync cells (white-black-white-black) then 20
// Gray-coded data cells, MSB first. Geometry is expressed as FRACTIONS of the
// frame, so the same arithmetic reads it at any resolution and after any
// scaling, and Gray coding bounds a torn mid-frame read to +-1.
//
// Two call sites need this and they are in different translation units — the
// decoded-frame check in ffmpeg.cpp and the scanned-out check in drm.cpp — so
// it lives here rather than being written twice. The painters
// (tests/hdmi-rx/engine/txctl.c, tests/stream/engine/synthsrc/synthd.c)
// and the host-side reader (tests/stream/engine/judges/barcode-decode.py)
// must agree with these constants.

#include <stdint.h>
#include <stddef.h>

#define FBC_CELLS         24
#define FBC_SYNC          4
#define FBC_DATA_BITS     20
#define FBC_MIN_CONTRAST  40

// Returns the frame index, or -1 if this frame carries no barcode — which is
// the honest answer for real console content and must never be reported as
// index 0. `stride` is bytes per row, `bytesPerPixel` selects the sample byte
// (1 = a luma plane, 3/4 = the green byte of a B,G,R[,A] pixel).
static inline long fbcDecode(const uint8_t* base, int stride, int w, int h,
                             int bytesPerPixel)
{
    if (base == nullptr || w < FBC_CELLS * 4 || h < 16 || stride < w) {
        return -1;
    }
    int stripH = h / 16;
    if (stripH < 8) {
        stripH = 8;
    }
    const uint8_t* row = base + (size_t)(stripH / 2) * (size_t)stride;

    int cells[FBC_CELLS];
    for (int i = 0; i < FBC_CELLS; i++) {
        int x0 = (int)(((long long)w * i) / FBC_CELLS);
        int x1 = (int)(((long long)w * (i + 1)) / FBC_CELLS);
        int cx = (x0 + x1) / 2;
        int sum = 0, n = 0;
        for (int dx = -2; dx <= 2; dx++) {
            int x = cx + dx;
            if (x >= 0 && x < w) {
                const uint8_t* px = row + (size_t)x * bytesPerPixel;
                sum += (bytesPerPixel >= 3) ? px[1] : px[0];
                n++;
            }
        }
        cells[i] = n ? sum / n : 0;
    }

    // The sync cells calibrate black and white for THIS frame, so the decode
    // survives limited range, codec ringing and any gain in the chain.
    int white = (cells[0] + cells[2]) / 2;
    int black = (cells[1] + cells[3]) / 2;
    if (white - black < FBC_MIN_CONTRAST) {
        return -1;
    }
    int thresh = (white + black) / 2;

    uint32_t gray = 0;
    for (int k = 0; k < FBC_DATA_BITS; k++) {
        if (cells[FBC_SYNC + (FBC_DATA_BITS - 1 - k)] > thresh) {
            gray |= (1u << k);
        }
    }
    uint32_t bin = gray;
    bin ^= bin >> 1;
    bin ^= bin >> 2;
    bin ^= bin >> 4;
    bin ^= bin >> 8;
    bin ^= bin >> 16;
    return (long)(bin & ((1u << FBC_DATA_BITS) - 1));
}
