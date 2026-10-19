/* halftone.c - 8bpp grey to 1bpp black/white.
 *
 * Halftone choice matters more than usual here.  The ML-1250 has 4 MB of RAM
 * and a 600 dpi A4 page is ~4.35 MB uncompressed, so the page only fits
 * because it is held compressed.  Error diffusion produces high-entropy noise
 * that defeats run-length compression and is the fastest way to provoke a
 * "print overrun" / memory error on this class of printer.  Clustered-dot
 * ordered dithering produces long runs and is the default for that reason.
 */

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ml1250.h"

/* Classic 8x8 clustered-dot (45 degree) matrix: dots grow outward from two
 * centres, so output is made of solid blobs -> long runs -> good compression.
 * 600 dpi / 8 = 75 lpi with 65 grey levels. */
static const uint8_t cluster8[64] = {
    24, 10, 12, 26, 35, 47, 49, 37,
     8,  0,  2, 14, 45, 59, 61, 51,
    22,  6,  4, 16, 43, 57, 63, 53,
    30, 20, 18, 28, 33, 41, 55, 39,
    34, 46, 48, 36, 25, 11, 13, 27,
    44, 58, 60, 50,  9,  1,  3, 15,
    42, 56, 62, 52, 23,  7,  5, 17,
    32, 40, 54, 38, 31, 21, 19, 29,
};

/* Bayer 8x8 dispersed-dot: finer detail, much worse compression. */
static const uint8_t bayer8[64] = {
     0, 32,  8, 40,  2, 34, 10, 42,
    48, 16, 56, 24, 50, 18, 58, 26,
    12, 44,  4, 36, 14, 46,  6, 38,
    60, 28, 52, 20, 62, 30, 54, 22,
     3, 35, 11, 43,  1, 33,  9, 41,
    51, 19, 59, 27, 49, 17, 57, 25,
    15, 47,  7, 39, 13, 45,  5, 37,
    63, 31, 55, 23, 61, 29, 53, 21,
};

halftone_t halftone_lookup(const char *name)
{
    if (!name)                              return HT_CLUSTER;
    if (strcasecmp(name, "cluster") == 0)   return HT_CLUSTER;
    if (strcasecmp(name, "bayer") == 0)     return HT_BAYER;
    if (strcasecmp(name, "fs") == 0 ||
        strcasecmp(name, "floyd") == 0)     return HT_FLOYD;
    if (strcasecmp(name, "threshold") == 0) return HT_THRESHOLD;
    die("unknown halftone %s (want cluster, bayer, fs or threshold)", name);
    return HT_CLUSTER;
}

static void setpix(bitmap_t *bm, int x, int y)
{
    bm->bits[(size_t)y * bm->stride + (x >> 3)] |= (uint8_t)(0x80u >> (x & 7));
}

static bitmap_t *ordered(const greymap_t *g, const uint8_t *m)
{
    bitmap_t *bm = bitmap_new(g->width, g->height, g->xdpi, g->ydpi);
    for (int y = 0; y < g->height; y++) {
        const uint8_t *src = g->grey + (size_t)y * g->width;
        const uint8_t *mrow = m + (y & 7) * 8;
        for (int x = 0; x < g->width; x++) {
            /* Threshold at (matrix + 0.5) scaled to 0..255.  src is 0=black. */
            int thr = (mrow[x & 7] * 255 + 32) / 64;
            if (src[x] <= thr) setpix(bm, x, y);
        }
    }
    return bm;
}

static bitmap_t *threshold50(const greymap_t *g)
{
    bitmap_t *bm = bitmap_new(g->width, g->height, g->xdpi, g->ydpi);
    for (int y = 0; y < g->height; y++) {
        const uint8_t *src = g->grey + (size_t)y * g->width;
        for (int x = 0; x < g->width; x++)
            if (src[x] < 128) setpix(bm, x, y);
    }
    return bm;
}

/* Floyd-Steinberg with serpentine traversal. */
static bitmap_t *floyd(const greymap_t *g)
{
    bitmap_t *bm = bitmap_new(g->width, g->height, g->xdpi, g->ydpi);
    int w = g->width;
    int *cur  = xcalloc((size_t)w + 2, sizeof *cur);
    int *next = xcalloc((size_t)w + 2, sizeof *next);

    for (int y = 0; y < g->height; y++) {
        const uint8_t *src = g->grey + (size_t)y * w;
        int left_to_right = (y & 1) == 0;
        memset(next, 0, ((size_t)w + 2) * sizeof *next);

        for (int i = 0; i < w; i++) {
            int x   = left_to_right ? i : w - 1 - i;
            int val = src[x] + cur[x + 1] / 16;
            if (val < 0) val = 0; else if (val > 255) val = 255;

            int out = val < 128 ? 0 : 255;
            if (out == 0) setpix(bm, x, y);
            int err = val - out;

            int fwd = left_to_right ? 1 : -1;
            /*      X   7
             *  3   5   1   / 16   (mirrored when travelling right-to-left) */
            cur [x + 1 + fwd] += err * 7;
            next[x + 1 - fwd] += err * 3;
            next[x + 1]       += err * 5;
            next[x + 1 + fwd] += err * 1;
        }
        int *t = cur; cur = next; next = t;
    }
    free(cur);
    free(next);
    return bm;
}

bitmap_t *halftone_apply(const greymap_t *g, halftone_t mode)
{
    switch (mode) {
    case HT_BAYER:     return ordered(g, bayer8);
    case HT_FLOYD:     return floyd(g);
    case HT_THRESHOLD: return threshold50(g);
    case HT_CLUSTER:
    default:           return ordered(g, cluster8);
    }
}
