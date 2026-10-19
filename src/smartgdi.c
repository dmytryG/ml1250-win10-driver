/* smartgdi.c - Samsung SmartGDI (SPL1) output.
 *
 * NOT for the ML-1250, which has a PCL controller.  This is for its siblings
 * from the same ML-1200 chassis - ML-1210, ML-1220M - and for the ML-4500 /
 * ML-5000A / ML-6040 family, all of which are host-based SmartGDI.  It is also
 * the fallback if a particular unit turns out to carry the GDI board.
 *
 * The wire format is reimplemented from Ghostscript's contrib/gdevgdi.c
 * ("SAMSUNG GDI driver for Ghostscript", GPL), which is the only public
 * description of it.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "ml1250.h"

#define GDI_BAND_HEIGHT   128     /* rows per band at 600 dpi, doubled at 300 */
#define GDI_MAX_BAND       66
#define GDI_COMP_MODITIFF   6
#define GDI_MAX_BAND_BYTES 0xfff7 /* the engine wedges past 64 KB per band */

/* Fixed raster widths in pixels: index 0 = A4, 1 = Letter/everything else. */
static const int gdi_band_width[2] = { 4768, 4928 };

/* Per-scanline "modified TIFF" run-length coding.
 *   0x00..0x3f          literal, (n+1) bytes follow           (n = code)
 *   0x40..0x7f          repeat,  (n+2) copies of next byte    (n = ~code & 0x7f)
 *   0x80nn..0xbfnn      long literal, (n+1) bytes follow      (n = code & 0x3fff)
 *   0xc0nn..0xffnn      long repeat,  (n+2) copies of next    (n = ~code & 0x3fff)
 * Repeat counts are stored inverted, literal counts are not. */
size_t gdi_pack_moditiff(uint8_t *dst, size_t dstcap, const uint8_t *src, size_t n)
{
    size_t i = 0, o = 0;

    while (i < n) {
        if (n - i == 1) {
            if (o + 2 > dstcap) return (size_t)-1;
            dst[o++] = 0x00;            /* literal of one */
            dst[o++] = src[i++];
            break;
        }

        if (src[i] == src[i + 1]) {
            size_t run = 2;
            while (i + run < n && src[i + run] == src[i]) run++;
            size_t cnt = run - 2;
            if (cnt >= 64) {
                unsigned v = (unsigned)((~cnt & 0x3fff) | 0xc000);
                if (o + 3 > dstcap) return (size_t)-1;
                dst[o++] = (uint8_t)(v >> 8);
                dst[o++] = (uint8_t)(v & 0xff);
            } else {
                if (o + 2 > dstcap) return (size_t)-1;
                dst[o++] = (uint8_t)(~cnt & 0x7f);
            }
            dst[o++] = src[i];
            i += run;
            continue;
        }

        /* Literal until a run of three identical bytes begins. */
        size_t start = i;
        while (i < n) {
            if (n - i >= 3 && src[i] == src[i + 1] && src[i + 1] == src[i + 2]) break;
            if (n - i == 2 && src[i] == src[i + 1]) break;
            i++;
        }
        size_t lit = i - start;
        size_t cnt = lit - 1;
        if (cnt >= 64) {
            unsigned v = (unsigned)((cnt & 0xbfff) | 0x8000);
            if (o + 2 > dstcap) return (size_t)-1;
            dst[o++] = (uint8_t)(v >> 8);
            dst[o++] = (uint8_t)(v & 0xff);
        } else {
            if (o + 1 > dstcap) return (size_t)-1;
            dst[o++] = (uint8_t)(cnt & 0x3f);
        }
        if (o + lit > dstcap) return (size_t)-1;
        memcpy(dst + o, src + start, lit);
        o += lit;
    }
    return o;
}

typedef struct { sink_t *s; int failed; } out_t;

static void emit(out_t *w, const void *p, size_t n)
{
    if (w->failed) return;
    if (sink_write(w->s, p, n) != 0) w->failed = 1;
}
static void emits(out_t *w, const char *s) { emit(w, s, strlen(s)); }
static void emitf(out_t *w, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) emit(w, buf, (size_t)n);
}

/* SmartGDI wraps each page in its own complete job, exactly as the original
 * driver did: header, bands, trailer, repeat. */
static void gdi_header(out_t *w, const jobopt_t *o, const paper_info_t *pi)
{
    emits(w, "\x1b%-12345X");
    emits(w, "@PJL SET PAPERTYPE = NORMAL ON\r\n");
    emitf(w, "@PJL SET DENSITY = %d\r\n", o->density > 0 ? o->density : 1);
    emitf(w, "@PJL SET TONERSAVE = %s\r\n", o->economode ? "ON" : "OFF");
    emits(w, "@PJL ENTER LANGUAGE = SMART\r\n");
    emits(w, "$PJL JOB START\r\n");
    emitf(w, "$PJL RESOLUTION = %d\r\n", o->ydpi == 300 ? 300 : 600);
    emitf(w, "$PJL COPIES = %d\r\n", o->copies > 0 ? o->copies : 1);
    /* The SMART language only knows these three page names. */
    if (o->paper == PAPER_A4)          emits(w, "$PJL PAGE A4 AUTO\r\n");
    else if (o->paper == PAPER_LEGAL)  emits(w, "$PJL PAGE LEGAL AUTO\r\n");
    else                               emits(w, "$PJL PAGE LETTER AUTO\r\n");
    (void)pi;
    emits(w, "$PJL BITMAP START\r\n");
}

static void gdi_trailer(out_t *w)
{
    emits(w, "$PJL PRINT 4\r\n");
    emits(w, "$PJL EOJ\r\n");
    emits(w, "$PJL SYNC\r\n");
    emits(w, "$PJL RELEASE 0 2047\r\n");
    emits(w, "$PJL GARBAGE\r\n");
    emits(w, "\x1b%-12345X\r\n");
}

static void band_header(out_t *w, unsigned bandno, unsigned comp,
                        unsigned width_bits, unsigned height, unsigned long size)
{
    uint8_t b[12];
    unsigned long total = size + 8;      /* the field counts 8 bytes of itself */
    b[0]  = (uint8_t)(total >> 24);
    b[1]  = (uint8_t)(total >> 16);
    b[2]  = (uint8_t)(total >> 8);
    b[3]  = (uint8_t)total;
    b[4]  = (uint8_t)(bandno >> 8);
    b[5]  = (uint8_t)bandno;
    b[6]  = (uint8_t)comp;
    b[7]  = 0;                           /* ubLeft */
    b[8]  = (uint8_t)(height >> 8);
    b[9]  = (uint8_t)height;
    b[10] = (uint8_t)(width_bits >> 8);
    b[11] = (uint8_t)width_bits;
    emit(w, b, sizeof b);
}

/* If a band will not compress below 64 KB the original driver progressively
 * rounds groups of bits up to solid, trading detail for a printable page. */
static void fudge_band(uint8_t *dst, const uint8_t *src, size_t n, int level)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t x = src[i], v = 0;
        if (level == 2) {
            v = (uint8_t)((((x >> 6) & 3) ? 3 : 0) << 6 | (((x >> 4) & 3) ? 3 : 0) << 4 |
                          (((x >> 2) & 3) ? 3 : 0) << 2 | (((x >> 0) & 3) ? 3 : 0));
        } else if (level == 3) {
            v = (uint8_t)((((x >> 4) & 0xf) ? 0xf : 0) << 4 | (((x >> 0) & 0xf) ? 0xf : 0));
        } else {
            v = x ? 0xff : 0x00;
        }
        dst[i] = v;
    }
}

static void write_page(out_t *w, const bitmap_t *bm, const jobopt_t *o)
{
    int paper_idx    = (o->paper == PAPER_A4) ? 0 : 1;
    int band_w_bytes = (gdi_band_width[paper_idx] + 31) / 32 * 4;
    int band_h       = GDI_BAND_HEIGHT;
    if (o->ydpi == 300) { band_w_bytes /= 2; band_h *= 2; }

    size_t band_size = (size_t)band_w_bytes * band_h;
    uint8_t *band  = xmalloc(band_size);
    uint8_t *alt   = xmalloc(band_size);
    size_t   ocap  = band_size * 13 / 10 + 64;
    uint8_t *obuf  = xmalloc(ocap);

    int copy = bm->stride < band_w_bytes ? bm->stride : band_w_bytes;
    int nbands = (bm->height + band_h - 1) / band_h;
    if (nbands > GDI_MAX_BAND) {
        warnf("page is %d bands but the engine accepts %d; truncating",
              nbands, GDI_MAX_BAND);
        nbands = GDI_MAX_BAND;
    }

    for (int b = 0; b < nbands && !w->failed; b++) {
        memset(band, 0, band_size);
        for (int j = 0; j < band_h; j++) {
            int y = b * band_h + j;
            if (y >= bm->height) break;
            memcpy(band + (size_t)j * band_w_bytes, bm->bits + (size_t)y * bm->stride, copy);
        }

        const uint8_t *use = band;
        size_t n = (size_t)-1;
        for (int level = 1; level <= 4; level++) {
            if (level > 1) { fudge_band(alt, band, band_size, level); use = alt; }
            n = 0;
            const uint8_t *p = use;
            int ok = 1;
            for (int j = 0; j < band_h; j++, p += band_w_bytes) {
                size_t k = gdi_pack_moditiff(obuf + n, ocap - n, p, (size_t)band_w_bytes);
                if (k == (size_t)-1) { ok = 0; break; }
                n += k;
            }
            if (ok && n <= GDI_MAX_BAND_BYTES) break;
            if (level == 4) { warnf("band %d still oversized after fudging", b); }
        }
        while (n % 4) obuf[n++] = 0x00;          /* pad to a 4-byte boundary */

        band_header(w, (unsigned)b, GDI_COMP_MODITIFF,
                    (unsigned)(band_w_bytes * 8), (unsigned)band_h, (unsigned long)n);
        emit(w, obuf, n);
    }

    free(band);
    free(alt);
    free(obuf);
}

int smartgdi_write_job(sink_t *s, bitmap_t **pages, int npages, const jobopt_t *o)
{
    out_t w = { s, 0 };
    const paper_info_t *pi = paper_by_id(o->paper);
    for (int i = 0; i < npages && !w.failed; i++) {
        gdi_header(&w, o, pi);
        write_page(&w, pages[i], o);
        gdi_trailer(&w);
    }
    return w.failed ? -1 : 0;
}
