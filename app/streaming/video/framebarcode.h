#ifndef FRAMEBARCODE_H
#define FRAMEBARCODE_H
/*
 * framebarcode.h — the per-frame index signal.
 *
 * SOURCE OF TRUTH. Every painter and every reader of the frame-index barcode
 * derives its geometry from this header:
 *
 *   painters   tests/hdmi-rx/engine/txctl.c        (HDMI loopback, RGB)
 *              tests/stream/engine/synthsrc/synthd.c (capture-socket bypass, NV12)
 *   readers    moonlight-qt-arm app/streaming/video/framebarcode.h (vendored copy)
 *                - decoded frames in ffmpeg.cpp, scanned-out pixels in drm.cpp
 *              tests/stream/engine/judges/barcode-decode.py (parses THIS file)
 *
 * The vendored copy in the Moonlight fork must stay byte-identical; `rigctl
 * doctor` checks it. Same arrangement, and the same reason, as colorpatch.h:
 * a painter and a reader that disagree about where bit 19 lives report a
 * PIPELINE fault, and the harness is the thing that is broken.
 *
 * Written to compile as BOTH C and C++ — the painters are C, the fork is C++.
 *
 * THE SIGNAL. A strip across the top 1/16 of every frame holds 24 cells: 4
 * sync cells (white-black-white-black) then 20 data cells carrying a Gray-coded
 * frame counter, MSB first.
 *
 * Geometry is FRACTIONAL, so one decoder reads the barcode at any resolution
 * and after any scaling — native capture, downscaled screenshot, client-side
 * decoded frame. The sync cells calibrate black and white per frame, so the
 * decode also survives limited range, codec ringing and gain anywhere in the
 * chain. Gray coding is what bounds a torn mid-frame sample to +-1 index
 * instead of an arbitrary bit-flip distance.
 *
 * LEVELS ARE DELIBERATELY NOT FIXED HERE. txctl paints full-range RGB
 * (0xFFFFFF / 0x000000) and synthd paints video-range luma (235 / 16), because
 * each is native to its source. Any level pair is legal as long as it still
 * clears FBC_MIN_CONTRAST at the reader after the codec.
 */

#include <stddef.h>

#define FBC_CELLS         24   /* 4 sync + 20 data                            */
#define FBC_SYNC          4
#define FBC_DATA_BITS     20
#define FBC_MIN_CONTRAST  40   /* sync white - sync black, 0..255             */
#define FBC_MASK          ((1u << FBC_DATA_BITS) - 1u)

/* The strip owns the top 1/16, never less than FBC_MIN_STRIP_PX rows so it
 * survives a heavy downscale. colorpatch.h starts its grid at the same
 * fraction (CP_TOP_NUM/CP_TOP_DEN) so the two signals never overlap. */
#define FBC_TOP_NUM       1
#define FBC_TOP_DEN       16
#define FBC_MIN_STRIP_PX  8

#define FBC_BEACON_PX     8    /* corner beacon, toggles on idx & 1           */
#define FBC_BAR_STEP_PX   8    /* moving-bar advance per frame                */

/* Strip height in rows for a frame of height h. */
static inline int fbc_strip_h(int h)
{
    int s = h * FBC_TOP_NUM / FBC_TOP_DEN;
    return s < FBC_MIN_STRIP_PX ? FBC_MIN_STRIP_PX : s;
}

/* Left edge of cell i. Cell edges tile the full width exactly, so there is no
 * leftover column at the right and cell_x(w, FBC_CELLS) == w. */
static inline int fbc_cell_x(int w, int i)
{
    return (int) (((long long) w * (long long) i) / FBC_CELLS);
}

/* Which cell carries bit k (k = 0 is the LSB): cell FBC_SYNC holds the MSB. */
static inline int fbc_bit_cell(int k)
{
    return FBC_SYNC + (FBC_DATA_BITS - 1 - k);
}

/* Frame index -> the Gray code actually painted, and back. */
static inline unsigned fbc_gray(unsigned idx)
{
    return (idx ^ (idx >> 1)) & FBC_MASK;
}

static inline unsigned fbc_ungray(unsigned g)
{
    g ^= g >> 1;
    g ^= g >> 2;
    g ^= g >> 4;
    g ^= g >> 8;
    g ^= g >> 16;
    return g & FBC_MASK;
}

/* Read the index out of a frame, or -1 if this frame carries no barcode —
 * which is the honest answer for real console content and must never be
 * reported as index 0. `stride` is bytes per row; `bytes_per_pixel` selects the
 * sample byte (1 = a luma plane, 3/4 = the green byte of a B,G,R[,A] pixel).
 *
 * Keeps the fork's camelCase: its three call sites (ffmpeg.cpp x2, drm.h)
 * already spell it this way, and the point of this header is that the vendored
 * copy is a byte-identical drop-in rather than a rename away. */
static inline long fbcDecode(const unsigned char *base, int stride, int w, int h,
                             int bytes_per_pixel)
{
    int strip_h, cells[FBC_CELLS];
    const unsigned char *row;
    int i, k, white, black, thresh;
    unsigned gray = 0;

    if (!base || w < FBC_CELLS * 4 || h < 16 || stride < w) {
        return -1;
    }
    strip_h = fbc_strip_h(h);
    row = base + (size_t) (strip_h / 2) * (size_t) stride;

    for (i = 0; i < FBC_CELLS; i++) {
        int cx = (fbc_cell_x(w, i) + fbc_cell_x(w, i + 1)) / 2;
        int sum = 0, n = 0, dx;
        /* Average a few samples across the cell centre: one pixel is at the
         * mercy of ringing, and the cell is wide enough to spare them. */
        for (dx = -2; dx <= 2; dx++) {
            int x = cx + dx;
            if (x >= 0 && x < w) {
                const unsigned char *px = row + (size_t) x * (size_t) bytes_per_pixel;
                sum += (bytes_per_pixel >= 3) ? px[1] : px[0];
                n++;
            }
        }
        cells[i] = n ? sum / n : 0;
    }

    white = (cells[0] + cells[2]) / 2;
    black = (cells[1] + cells[3]) / 2;
    if (white - black < FBC_MIN_CONTRAST) {
        return -1;
    }
    thresh = (white + black) / 2;

    for (k = 0; k < FBC_DATA_BITS; k++) {
        if (cells[fbc_bit_cell(k)] > thresh) {
            gray |= (1u << k);
        }
    }
    return (long) fbc_ungray(gray);
}

#endif /* FRAMEBARCODE_H */
