#ifndef COLORPATCH_H
#define COLORPATCH_H
/*
 * colorpatch.h — the colour-accuracy patch set.
 *
 * SOURCE OF TRUTH. The painter (stability/hdmi-rx/engine/txctl.c) and the
 * checker (moonlight-qt-arm app/streaming/video/colorpatch.h, a vendored copy)
 * both include this. The two copies must stay byte-identical: the whole test
 * rests on the painter and the checker agreeing on what colour a patch is, and
 * a silent disagreement would report the harness as a pipeline fault. Same
 * arrangement as retro-overlay-protocol.h.
 *
 * Written to compile as BOTH C and C++ — the painter is C, the checker is C++.
 *
 * WHY PATCHES AND NOT PIXELS. The chain converts to NV12 for encode, so chroma
 * is averaged over 2x2, and HEVC then quantises. A frame carrying one colour
 * per pixel measures the subsampler and the codec, not the colour pipeline. A
 * UNIFORM BLOCK's chroma survives 4:2:0 intact, so sampling well inside a block
 * makes the round trip lossless in a correct chain — which is what makes it a
 * valid oracle. Blocks are also why exhaustive coverage stays affordable: at
 * 16x16 the whole 8-bit cube is 518 frames, 8.6 s at 60 fps. See
 * ../../color/README.md.
 */

#define CP_COLS 8
#define CP_ROWS 4
#define CP_PATCHES (CP_COLS * CP_ROWS)

/* The frame-index barcode owns the top 1/16 (framebarcode.h); the patch grid
 * starts below it so the two never overlap. */
#define CP_TOP_NUM 1
#define CP_TOP_DEN 16

/* Expected RGB, full range, BT.709 primaries.
 *
 * Row 0/1 are the colour bars: the primaries and secondaries are where a wrong
 * conversion MATRIX shows itself, because BT.601 and BT.709 differ most on
 * saturated colour and barely at all on grey — a chain running 601 where it
 * declared 709 looks perfect on a ramp.
 * Row 2 is the ramp, which is where a wrong TRANSFER or gain shows.
 * Row 3 straddles the limited-range rails (16 and 235) so clipping is visible
 * as such rather than as a vague error: if 1 and 8 come back equal to 16, the
 * chain crushed them.
 */
static const unsigned char cp_patch_rgb[CP_PATCHES][3] = {
    /* row 0 — BT.709 75 % colour bars */
    {191, 191, 191}, {191, 191, 0}, {0, 191, 191}, {0, 191, 0},
    {191, 0, 191},   {191, 0, 0},   {0, 0, 191},   {0, 0, 0},
    /* row 1 — 100 % bars: primaries and secondaries at full amplitude */
    {255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
    {255, 0, 255},   {255, 0, 0},   {0, 0, 255},   {0, 0, 0},
    /* row 2 — greyscale ramp */
    {0, 0, 0},       {36, 36, 36},  {73, 73, 73},  {109, 109, 109},
    {146, 146, 146}, {182, 182, 182}, {219, 219, 219}, {255, 255, 255},
    /* row 3 — range probes around the limited-range rails */
    {1, 1, 1},       {8, 8, 8},     {16, 16, 16},  {17, 17, 17},
    {234, 234, 234}, {235, 235, 235}, {236, 236, 236}, {254, 254, 254},
};

/* Indices worth naming, because the judge reports on them individually. */
#define CP_IDX_BLACK   16  /* row 2, ramp 0   */
#define CP_IDX_WHITE   23  /* row 2, ramp 255 */
#define CP_IDX_RAMP0   16
#define CP_IDX_RAMP7   23

/* Full patch rectangle. Geometry is FRACTIONAL so the same arithmetic addresses
 * the grid at any resolution and after any scaling. */
static inline void cp_patch_rect(int w, int h, int idx,
                          int *x0, int *y0, int *x1, int *y1)
{
    int col = idx % CP_COLS;
    int row = idx / CP_COLS;
    int gy0 = (int) ((long long) h * CP_TOP_NUM / CP_TOP_DEN);
    int gh = h - gy0;
    *x0 = (int) ((long long) w * col / CP_COLS);
    *x1 = (int) ((long long) w * (col + 1) / CP_COLS);
    *y0 = gy0 + (int) ((long long) gh * row / CP_ROWS);
    *y1 = gy0 + (int) ((long long) gh * (row + 1) / CP_ROWS);
}

/* Which patch covers this pixel, or -1 for the barcode strip above the grid.
 *
 * O(1) on purpose. The obvious implementation — walk the 32 patches asking
 * cp_patch_rect whether each contains the pixel — is O(32) PER PIXEL, and both
 * painters call this for every luma and chroma sample of every pool buffer.
 * Measured as ~265 million rect computations to paint one 1080p pool, which
 * took long enough at SETUP that Sunshine's encoder probe gave up and logged
 * "Unable to find display or encoder during startup". Bucketing is the exact
 * inverse of cp_patch_rect's boundaries, so the two cannot disagree. */
static inline int cp_patch_at(int w, int h, int x, int y)
{
    int gy0 = (int) ((long long) h * CP_TOP_NUM / CP_TOP_DEN);
    int gh = h - gy0;
    int col, row;
    if (y < gy0 || gh <= 0 || w <= 0) {
        return -1;
    }
    col = (int) ((long long) x * CP_COLS / w);
    row = (int) ((long long) (y - gy0) * CP_ROWS / gh);
    if (col < 0) col = 0;
    if (col >= CP_COLS) col = CP_COLS - 1;
    if (row < 0) row = 0;
    if (row >= CP_ROWS) row = CP_ROWS - 1;
    return row * CP_COLS + col;
}

/* The region actually SAMPLED: the middle half of the patch in each axis. The
 * outer quarter is given up deliberately — deblocking and DCT ringing bleed
 * across a patch edge, and chroma siting puts the first and last samples of a
 * 4:2:0 block astride the boundary. Measuring the middle measures the colour;
 * measuring the edge measures the codec. */
static inline void cp_sample_box(int w, int h, int idx,
                          int *x0, int *y0, int *x1, int *y1)
{
    int px0, py0, px1, py1, qw, qh;
    cp_patch_rect(w, h, idx, &px0, &py0, &px1, &py1);
    qw = (px1 - px0) / 4;
    qh = (py1 - py0) / 4;
    *x0 = px0 + qw;
    *x1 = px1 - qw;
    *y0 = py0 + qh;
    *y1 = py1 - qh;
    /* Degenerate geometry (a tiny frame) still has to yield a usable box. */
    if (*x1 <= *x0) { *x0 = px0; *x1 = px1; }
    if (*y1 <= *y0) { *y0 = py0; *y1 = py1; }
}

#endif /* COLORPATCH_H */
