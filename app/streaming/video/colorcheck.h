#pragma once

// colorcheck.h — verify, ON THE CLIENT, that the colours arriving are the
// colours the source painted.
//
// The point of doing it here is that the client is the only place that sees
// what the whole chain produced: RGB source -> HDMI-TX -> HDMI-RX -> capture
// format conversion -> RGA/GL -> HEVC -> network -> decode. A host-side check
// would exonerate half of that by construction.
//
// Method, per patch (colorpatch.h defines where they are and what they should
// be): average Y and CbCr well inside the patch, convert to RGB, and compare
// with CIEDE2000. dE2000 is used rather than a per-channel difference because
// the question is "would a person see this", and 8 code values of error in
// blue is not the same defect as 8 in green.
//
// Two failure modes are separated on purpose:
//   * RANGE. Y for the black and white ramp patches says whether the chain is
//     limited (16/235) or full (0/255), and whether it CLIPPED — if the 1, 8
//     and 16 probes come back equal, blacks were crushed. Range is measured and
//     reported, then used for the conversion, so a legitimate limited-range
//     chain does not read as 16 code values of error on every patch.
//   * MATRIX. With range handled, what remains on saturated colour is the
//     conversion matrix. BT.601 where BT.709 was declared is nearly invisible
//     on a grey ramp and glaring on primaries, which is exactly why the patch
//     set carries both.

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "colorpatch.h"

struct CcPatchResult {
    int idx;
    int r, g, b;        // what arrived, converted to full-range RGB
    double de;          // CIEDE2000 against cp_patch_rgb[idx]
    int y, u, v;        // raw sampled YCbCr, for diagnosis
};

struct CcResult {
    bool ok;
    int patches;
    int y_black, y_white;   // sampled Y of the ramp endpoints
    bool limited_range;
    bool clipped_black;     // the 1/8/16 probes are indistinguishable
    bool clipped_white;     // the 235/236/254 probes are indistinguishable
    double de_mean, de_p95, de_max;
    int worst_idx;
    CcPatchResult patch[CP_PATCHES];
};

// ---------------------------------------------------------------- colour math
static inline double cc_srgb_to_linear(double c)
{
    c /= 255.0;
    return (c <= 0.04045) ? (c / 12.92) : pow((c + 0.055) / 1.055, 2.4);
}

static inline double cc_lab_f(double t)
{
    const double d = 6.0 / 29.0;
    return (t > d * d * d) ? cbrt(t) : (t / (3.0 * d * d) + 4.0 / 29.0);
}

// sRGB/BT.709 primaries, D65. The transfer function is sRGB's; both sides of
// the comparison use the same one, so this is a round-trip identity test and
// any consistent choice is valid.
static inline void cc_rgb_to_lab(double R, double G, double B, double *L,
                                 double *a, double *bb)
{
    double r = cc_srgb_to_linear(R), g = cc_srgb_to_linear(G), b = cc_srgb_to_linear(B);
    double X = 0.4124564 * r + 0.3575761 * g + 0.1804375 * b;
    double Y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b;
    double Z = 0.0193339 * r + 0.1191920 * g + 0.9503041 * b;
    double fx = cc_lab_f(X / 0.95047), fy = cc_lab_f(Y / 1.00000), fz = cc_lab_f(Z / 1.08883);
    *L = 116.0 * fy - 16.0;
    *a = 500.0 * (fx - fy);
    *bb = 200.0 * (fy - fz);
}

// CIEDE2000 (Sharma et al.'s formulation).
static inline double cc_de2000(double L1, double a1, double b1,
                               double L2, double a2, double b2)
{
    const double PI = 3.14159265358979323846;
    double C1 = sqrt(a1 * a1 + b1 * b1), C2 = sqrt(a2 * a2 + b2 * b2);
    double Cbar = (C1 + C2) / 2.0;
    double Cbar7 = pow(Cbar, 7.0);
    double G = 0.5 * (1.0 - sqrt(Cbar7 / (Cbar7 + pow(25.0, 7.0))));
    double a1p = (1.0 + G) * a1, a2p = (1.0 + G) * a2;
    double C1p = sqrt(a1p * a1p + b1 * b1), C2p = sqrt(a2p * a2p + b2 * b2);
    double h1p = (b1 == 0.0 && a1p == 0.0) ? 0.0 : atan2(b1, a1p) * 180.0 / PI;
    double h2p = (b2 == 0.0 && a2p == 0.0) ? 0.0 : atan2(b2, a2p) * 180.0 / PI;
    if (h1p < 0) h1p += 360.0;
    if (h2p < 0) h2p += 360.0;

    double dLp = L2 - L1;
    double dCp = C2p - C1p;
    double dhp;
    if (C1p * C2p == 0.0) {
        dhp = 0.0;
    } else if (fabs(h2p - h1p) <= 180.0) {
        dhp = h2p - h1p;
    } else if (h2p - h1p > 180.0) {
        dhp = h2p - h1p - 360.0;
    } else {
        dhp = h2p - h1p + 360.0;
    }
    double dHp = 2.0 * sqrt(C1p * C2p) * sin(dhp * PI / 360.0);

    double Lbarp = (L1 + L2) / 2.0;
    double Cbarp = (C1p + C2p) / 2.0;
    double hbarp;
    if (C1p * C2p == 0.0) {
        hbarp = h1p + h2p;
    } else if (fabs(h1p - h2p) <= 180.0) {
        hbarp = (h1p + h2p) / 2.0;
    } else if (h1p + h2p < 360.0) {
        hbarp = (h1p + h2p + 360.0) / 2.0;
    } else {
        hbarp = (h1p + h2p - 360.0) / 2.0;
    }

    double T = 1.0 - 0.17 * cos((hbarp - 30.0) * PI / 180.0)
                   + 0.24 * cos((2.0 * hbarp) * PI / 180.0)
                   + 0.32 * cos((3.0 * hbarp + 6.0) * PI / 180.0)
                   - 0.20 * cos((4.0 * hbarp - 63.0) * PI / 180.0);
    double dtheta = 30.0 * exp(-pow((hbarp - 275.0) / 25.0, 2.0));
    double Cbarp7 = pow(Cbarp, 7.0);
    double RC = 2.0 * sqrt(Cbarp7 / (Cbarp7 + pow(25.0, 7.0)));
    double Lm50 = (Lbarp - 50.0) * (Lbarp - 50.0);
    double SL = 1.0 + (0.015 * Lm50) / sqrt(20.0 + Lm50);
    double SC = 1.0 + 0.045 * Cbarp;
    double SH = 1.0 + 0.015 * Cbarp * T;
    double RT = -sin(2.0 * dtheta * PI / 180.0) * RC;

    double tL = dLp / SL, tC = dCp / SC, tH = dHp / SH;
    return sqrt(tL * tL + tC * tC + tH * tH + RT * tC * tH);
}

static inline double cc_clamp255(double v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

// ------------------------------------------------------------------ sampling
// Average a plane over the sample box. Sub-sampled on a fixed lattice rather
// than every pixel: a patch is ~120x126 at 1080p and averaging 16x16 of it is
// already far below the noise the codec introduces.
static inline void cc_avg(const uint8_t *base, int pitch, int x0, int y0,
                          int x1, int y1, int stepBytes, double *out0, double *out1)
{
    const int N = 16;
    double s0 = 0, s1 = 0;
    int n = 0;
    for (int i = 0; i < N; i++) {
        int y = y0 + (y1 - y0) * (2 * i + 1) / (2 * N);
        const uint8_t *row = base + (size_t) y * pitch;
        for (int j = 0; j < N; j++) {
            int x = x0 + (x1 - x0) * (2 * j + 1) / (2 * N);
            s0 += row[(size_t) x * stepBytes];
            if (out1) s1 += row[(size_t) x * stepBytes + 1];
            n++;
        }
    }
    *out0 = n ? s0 / n : 0;
    if (out1) *out1 = n ? s1 / n : 0;
}

// NV12 in, verdict out. `yb`/`yw` are measured first because the conversion
// depends on them.
static inline bool ccCheckNV12(const uint8_t *y, int yPitch,
                               const uint8_t *uv, int uvPitch,
                               int w, int h, CcResult *res)
{
    if (!y || !uv || w < CP_COLS * 8 || h < CP_ROWS * 8) {
        return false;
    }
    memset(res, 0, sizeof(*res));
    res->patches = CP_PATCHES;

    double sy[CP_PATCHES], su[CP_PATCHES], sv[CP_PATCHES];
    for (int i = 0; i < CP_PATCHES; i++) {
        int x0, y0, x1, y1, dummy;
        cp_sample_box(w, h, i, &x0, &y0, &x1, &y1);
        cc_avg(y, yPitch, x0, y0, x1, y1, 1, &sy[i], NULL);
        // Chroma is half resolution in both axes; the same box in chroma
        // coordinates is the same region of the picture.
        cc_avg(uv, uvPitch, x0 / 2, y0 / 2, x1 / 2, y1 / 2, 2, &su[i], &sv[i]);
        (void) dummy;
    }

    res->y_black = (int) lround(sy[CP_IDX_BLACK]);
    res->y_white = (int) lround(sy[CP_IDX_WHITE]);
    // Halfway between the two conventions is an unambiguous discriminator: a
    // limited-range black sits near 16, a full-range one near 0.
    res->limited_range = (res->y_black > 8);
    // Row 3 probes: 1, 8, 16 (indices 24,25,26) and 235, 236, 254 (29,30,31).
    res->clipped_black = (fabs(sy[24] - sy[26]) < 1.5 && fabs(sy[25] - sy[26]) < 1.5);
    res->clipped_white = (fabs(sy[31] - sy[29]) < 1.5 && fabs(sy[30] - sy[29]) < 1.5);

    const double yOff = res->limited_range ? 16.0 : 0.0;
    const double yScale = res->limited_range ? (255.0 / 219.0) : 1.0;
    const double cScale = res->limited_range ? (255.0 / 224.0) : 1.0;

    double sum = 0;
    double des[CP_PATCHES];
    for (int i = 0; i < CP_PATCHES; i++) {
        double Y = (sy[i] - yOff) * yScale;
        double Cb = (su[i] - 128.0) * cScale;
        double Cr = (sv[i] - 128.0) * cScale;
        // BT.709 inverse.
        double R = Y + 1.5748 * Cr;
        double G = Y - 0.1873 * Cb - 0.4681 * Cr;
        double B = Y + 1.8556 * Cb;
        R = cc_clamp255(R); G = cc_clamp255(G); B = cc_clamp255(B);

        double L1, a1, b1, L2, a2, b2;
        cc_rgb_to_lab(R, G, B, &L1, &a1, &b1);
        cc_rgb_to_lab(cp_patch_rgb[i][0], cp_patch_rgb[i][1], cp_patch_rgb[i][2],
                      &L2, &a2, &b2);
        double de = cc_de2000(L1, a1, b1, L2, a2, b2);

        res->patch[i].idx = i;
        res->patch[i].r = (int) lround(R);
        res->patch[i].g = (int) lround(G);
        res->patch[i].b = (int) lround(B);
        res->patch[i].y = (int) lround(sy[i]);
        res->patch[i].u = (int) lround(su[i]);
        res->patch[i].v = (int) lround(sv[i]);
        res->patch[i].de = de;
        des[i] = de;
        sum += de;
        if (de > res->de_max) { res->de_max = de; res->worst_idx = i; }
    }
    res->de_mean = sum / CP_PATCHES;

    double sorted[CP_PATCHES];
    memcpy(sorted, des, sizeof(sorted));
    for (int i = 1; i < CP_PATCHES; i++) {          // insertion sort, n=32
        double k = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > k) { sorted[j + 1] = sorted[j]; j--; }
        sorted[j + 1] = k;
    }
    res->de_p95 = sorted[(int) (CP_PATCHES * 95 / 100)];
    res->ok = true;
    return true;
}
