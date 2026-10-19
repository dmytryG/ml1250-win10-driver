/* pcl5.c - PCL5e monochrome raster generation.
 *
 * This is the primary output mode.  The ML-1250 advertises PCL6 but exposes a
 * PCL 5e menu on its control panel and Samsung's own Linux instructions say to
 * pick "Samsung Printer PCL 5e Compatible", so PCL5e raster is both supported
 * and far more conservative than the PCL-XL that modern class drivers emit.
 *
 * Everything here is deliberately plain: no fonts, no macros, no palettes,
 * just a positioned raster block.  That is exactly the subset a year-2000
 * interpreter implements without surprises.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "ml1250.h"

#define ESC "\x1b"
#define UEL ESC "%-12345X"

/* -------------------------------------------------------- compression */

/* PCL compression method 2: TIFF "PackBits" run-length.
 *   0..127   -> literal run, count+1 bytes follow
 *   129..255 -> repeated run, (257-count) copies of the next byte
 *   128      -> no-op, never emitted
 * Stateless per row, which is why it is the safe default: unlike delta-row
 * (method 3) it carries no seed-row that a Y-offset might invalidate. */
size_t pcl_pack_tiff(uint8_t *dst, size_t dstcap, const uint8_t *src, size_t n)
{
    size_t i = 0, o = 0;

    while (i < n) {
        /* How long is the run starting here? */
        size_t run = 1;
        while (i + run < n && src[i + run] == src[i] && run < 128) run++;

        if (run >= 2) {
            if (o + 2 > dstcap) return (size_t)-1;
            dst[o++] = (uint8_t)(257 - run);
            dst[o++] = src[i];
            i += run;
            continue;
        }

        /* Literal run: consume bytes until a run of 3+ starts (a run of 2 is
         * not worth breaking a literal for). */
        size_t start = i, lit = 0;
        while (i < n && lit < 128) {
            size_t r = 1;
            while (i + r < n && src[i + r] == src[i] && r < 3) r++;
            if (r >= 3) break;
            i++;
            lit++;
        }
        if (o + 1 + lit > dstcap) return (size_t)-1;
        dst[o++] = (uint8_t)(lit - 1);
        memcpy(dst + o, src + start, lit);
        o += lit;
    }
    return o;
}

/* PCL compression method 3: delta row.  Each command is
 *   control byte: bits 7..5 = (bytes to replace - 1), bits 4..0 = relative
 *                 offset from the last untouched byte; offset 31 means
 *                 "more offset bytes follow, each 0..255, continuing while
 *                 the byte is 255"
 *   then the replacement bytes.
 * A row identical to the seed row encodes as zero bytes. */
size_t pcl_pack_delta(uint8_t *dst, size_t dstcap, const uint8_t *cur,
                      const uint8_t *prev, size_t n)
{
    size_t o = 0, i = 0, last = 0;   /* last = index just past previous change */

    while (i < n) {
        if (cur[i] == prev[i]) { i++; continue; }

        size_t start = i;
        /* Gather up to 8 changed bytes.  Allow a single matching byte inside a
         * group only if a change follows, which keeps groups dense. */
        size_t run = 0;
        while (i < n && run < 8) {
            if (cur[i] != prev[i]) { i++; run++; continue; }
            size_t look = i;
            while (look < n && cur[look] == prev[look] && look - i < 2) look++;
            if (look < n && look - i < 2 && cur[look] != prev[look] && run + (look - i) < 8) {
                run += look - i;
                i = look;
                continue;
            }
            break;
        }

        size_t offset = start - last;
        if (o + 1 > dstcap) return (size_t)-1;
        if (offset < 31) {
            dst[o++] = (uint8_t)(((run - 1) << 5) | offset);
        } else {
            dst[o++] = (uint8_t)(((run - 1) << 5) | 31);
            offset -= 31;
            while (offset >= 255) {
                if (o + 1 > dstcap) return (size_t)-1;
                dst[o++] = 255;
                offset -= 255;
            }
            if (o + 1 > dstcap) return (size_t)-1;
            dst[o++] = (uint8_t)offset;
        }
        if (o + run > dstcap) return (size_t)-1;
        memcpy(dst + o, cur + start, run);
        o += run;
        last = i;
    }
    return o;
}

/* ------------------------------------------------------------- emitting */

typedef struct {
    sink_t *s;
    int     failed;
} out_t;

static void emit(out_t *w, const void *p, size_t n)
{
    if (w->failed) return;
    if (sink_write(w->s, p, n) != 0) w->failed = 1;
}

static void emits(out_t *w, const char *s) { emit(w, s, strlen(s)); }

static void emitf(out_t *w, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) emit(w, buf, (size_t)n);
}

/* ------------------------------------------------------------------ job */

static void pjl_header(out_t *w, const jobopt_t *o)
{
    if (!o->pjl) return;
    /* The UEL must be followed immediately by @PJL on the same line. */
    emits(w, UEL);
    emitf(w, "@PJL JOB NAME=\"%s\"\r\n", o->jobname ? o->jobname : "ml1250");
    emitf(w, "@PJL SET RESOLUTION=%d\r\n", o->ydpi);
    emitf(w, "@PJL SET ECONOMODE=%s\r\n", o->economode ? "ON" : "OFF");
    if (o->density > 0)
        emitf(w, "@PJL SET DENSITY=%d\r\n", o->density);
    /* Samsung calls this SRT on the front panel; in PJL it is plain RET,
     * with the tokens NORMAL, ENHANCE, GRAY and GRAY+. */
    if (o->ret)
        emitf(w, "@PJL SET RET=%s\r\n", o->ret);
    /* Not sent by default.  Page protection needs a full page buffer, which
     * a stock 4 MB ML-1250 does not have, and Samsung's own driver does not
     * send it either.  Ask for it explicitly if the printer has more RAM. */
    if (o->pageprotect)
        emitf(w, "@PJL SET PAGEPROTECT=%s\r\n", o->pageprotect);
    if (o->copies > 1)
        emitf(w, "@PJL SET COPIES=%d\r\n", o->copies);
    emits(w, "@PJL ENTER LANGUAGE=PCL\r\n");
}

static void pjl_trailer(out_t *w, const jobopt_t *o)
{
    emits(w, ESC "E");                  /* reset, flushes the last page */
    if (!o->pjl) return;
    emits(w, UEL);
    emits(w, "@PJL EOJ\r\n");
    emits(w, UEL);
}

static void page_setup(out_t *w, const jobopt_t *o, const paper_info_t *pi)
{
    emits(w, ESC "E");                        /* printer reset */
    if (pi->pcl_code)
        emitf(w, ESC "&l%dA", pi->pcl_code);  /* page size */
    emits(w, ESC "&l0O");                     /* portrait */
    emits(w, ESC "&l0E");                     /* top margin 0 lines */
    emits(w, ESC "&l0L");                     /* perforation skip off */
    if (o->copies > 1 && !o->pjl)
        emitf(w, ESC "&l%dX", o->copies);
    emitf(w, ESC "&u%dD", o->xdpi);           /* PCL unit of measure = dots */
}

/* Cost in bytes of switching compression method mid-image: "<Esc>*b#M". */
#define MODE_SWITCH_COST 5

/* Below this many consecutive blank rows it is cheaper to send the rows than
 * to emit a Y offset - and it avoids the seed-row reset that some printers
 * get wrong.  Ghostscript's ljet4 device uses the same threshold. */
#define MIN_SKIP_LINES 7

/* Writes one page of raster.  x0/y0 are in dots from the origin of the PCL
 * logical page. */
static void write_page(out_t *w, const bitmap_t *bm, const jobopt_t *o,
                       int x0, int y0)
{
    int stride = bm->stride;
    size_t cap = (size_t)stride * 2 + 32;
    uint8_t *buf2 = xmalloc(cap);
    uint8_t *buf3 = xmalloc(cap);
    uint8_t *prev = xcalloc((size_t)stride, 1);
    uint8_t *zrow = xcalloc((size_t)stride, 1);
    uint8_t *zpak = xmalloc(cap);
    size_t   zlen = pcl_pack_tiff(zpak, cap, zrow, (size_t)stride);

    int fixed = o->compression;                 /* -1 means choose per row */
    if (fixed != -1 && fixed != 0 && fixed != 2 && fixed != 3) fixed = 2;

    /* Raster geometry has to be set before entering graphics mode: once
     * <Esc>*r#A has been seen, only data, compression and Y offset are
     * accepted and everything else implicitly ends the image. */
    emitf(w, ESC "*t%dR", o->xdpi);       /* raster resolution             */
    emits(w, ESC "*r0F");                 /* raster follows logical page   */
    emitf(w, ESC "*r%dS", bm->width);     /* source raster width  (pixels) */
    emitf(w, ESC "*r%dT", bm->height);    /* source raster height (rows)   */
    emitf(w, ESC "*p%dx%dY", x0, y0);     /* cursor to top-left of image   */
    emits(w, ESC "*r1A");                 /* start raster at the cursor    */

    /* <Esc>*r#A resets the compression method to 0, so it must be selected
     * after entering graphics mode, not before. */
    int cur = -1;

    int pending_blank = 0;
    for (int y = 0; y < bm->height && !w->failed; y++) {
        const uint8_t *row = bm->bits + (size_t)y * stride;

        if (bitmap_row_blank(bm, y)) { pending_blank++; continue; }

        if (pending_blank >= MIN_SKIP_LINES) {
            emitf(w, ESC "*b%dY", pending_blank);
            /* The skipped rows are white, so the delta seed becomes zeros. */
            memset(prev, 0, (size_t)stride);
            pending_blank = 0;
        }
        /* A short blank run is cheaper sent as data than as a Y offset. */
        if (pending_blank > 0) {
            if (cur != 2) { emits(w, ESC "*b2M"); cur = 2; }
            for (; pending_blank > 0; pending_blank--) {
                emitf(w, ESC "*b%zuW", zlen);
                emit(w, zpak, zlen);
            }
            memset(prev, 0, (size_t)stride);   /* last row sent was blank */
        }

        size_t n2 = (size_t)-1, n3 = (size_t)-1;
        if (fixed == -1 || fixed == 2)
            n2 = pcl_pack_tiff(buf2, cap, row, (size_t)stride);
        if (fixed == -1 || fixed == 3)
            n3 = pcl_pack_delta(buf3, cap, row, prev, (size_t)stride);

        int want;
        const uint8_t *data;
        size_t n;
        if (fixed == 0) {
            want = 0; data = row; n = (size_t)stride;
        } else if (fixed == 2) {
            want = 2; data = buf2; n = n2;
        } else if (fixed == 3) {
            want = 3; data = buf3; n = n3;
        } else {
            /* Charge each candidate the cost of switching into its mode. */
            size_t c2 = (n2 == (size_t)-1) ? (size_t)-1
                                           : n2 + (cur == 2 ? 0 : MODE_SWITCH_COST);
            size_t c3 = (n3 == (size_t)-1) ? (size_t)-1
                                           : n3 + (cur == 3 ? 0 : MODE_SWITCH_COST);
            if (c3 < c2) { want = 3; data = buf3; n = n3; }
            else         { want = 2; data = buf2; n = n2; }
        }

        if (n == (size_t)-1) { want = 0; data = row; n = (size_t)stride; }
        if (want != cur) { emitf(w, ESC "*b%dM", want); cur = want; }
        emitf(w, ESC "*b%zuW", n);
        if (n) emit(w, data, n);

        memcpy(prev, row, (size_t)stride);
    }

    emits(w, ESC "*rC");                  /* end raster graphics */
    emits(w, "\f");                       /* eject the sheet     */

    free(buf2);
    free(buf3);
    free(prev);
    free(zrow);
    free(zpak);
}

int pcl5_write_job(sink_t *s, bitmap_t **pages, int npages, const jobopt_t *o)
{
    out_t w = { s, 0 };
    const paper_info_t *pi = paper_by_id(o->paper);

    pjl_header(&w, o);
    page_setup(&w, o, pi);

    /* PCL cursor (0,0) is the top-left of the *logical* page, whose left edge
     * already coincides with the printable left edge.  Vertically the logical
     * page starts at the physical top edge, so the image has to be pushed
     * down past the unprintable band or the printer clips its first rows. */
    int px, py;
    paper_printable(pi, o->ydpi, &px, &py, NULL, NULL);

    for (int i = 0; i < npages && !w.failed; i++) {
        if (i > 0) page_setup(&w, o, pi);
        write_page(&w, pages[i], o, o->offset_x, py + o->offset_y);
    }

    pjl_trailer(&w, o);
    return w.failed ? -1 : 0;
}
