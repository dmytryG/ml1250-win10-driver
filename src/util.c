/* util.c - allocation, diagnostics, paper tables, bitmap/greymap helpers. */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ml1250.h"

int verbose = 0;

void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("ml1250: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

void warnf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("ml1250: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory (%zu bytes)", n);
    return p;
}

void *xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) die("out of memory (%zu x %zu bytes)", n, sz);
    return p;
}

/* ----------------------------------------------------------------- paper */

/* pcl_code is the <Esc>&l#A page-size value (0 = printer default, omit the
 * command).  pclxl_media is the eMediaSize enumeration from the PCL XL
 * Feature Reference, as tabulated in Microsoft's shipping PCLXL.GPD:
 * eLetterPaper=0, eLegalPaper=1, eA4Paper=2, eExecPaper=3, eLedgerPaper=4,
 * eA3Paper=5, eJB4Paper=0x0a, eJB5Paper=0x0b, eA5Paper=0x10, eA6Paper=0x11. */
/* phys/logical/offset columns are straight out of the PCL 5 Technical
 * Reference "Portrait logical page and printable area" table, at 300 dpi.
 * The left inset is always (physical - logical) / 2, the top offset is always
 * zero, and the unprintable band at top and bottom is always 1/6 inch.
 * A5, JIS B5 and Folio are not in HP's table; they follow the same rule with
 * the ISO inset (71) for A5 and the US inset (75) for the other two. */
static const paper_info_t papers[] = {
    /* name        w_in    h_in    pcl  pclxl  physW physH  logW  left vmarg */
    { "a4",        8.2677, 11.6929, 26, 0x02, 2480, 3507, 2338,  71,  50 },
    { "letter",    8.5000, 11.0000,  2, 0x00, 2550, 3300, 2400,  75,  50 },
    { "legal",     8.5000, 14.0000,  3, 0x01, 2550, 4200, 2400,  75,  50 },
    { "executive", 7.2500, 10.5000,  1, 0x03, 2175, 3150, 2025,  75,  50 },
    { "a5",        5.8268,  8.2677, 25, 0x10, 1748, 2480, 1606,  71,  50 },
    { "b5",        7.1654, 10.1181, 45, 0x0b, 2150, 3035, 2008,  71,  50 },
    { "folio",     8.5000, 13.0000,  0, 0x01, 2550, 3900, 2400,  75,  50 },
};

void paper_printable(const paper_info_t *pi, int dpi, int *x0, int *y0,
                     int *w, int *h)
{
    /* The table is at 300 dpi; every value scales linearly. */
    int s = dpi / 300;
    if (s < 1) s = 1;
    if (x0) *x0 = pi->left_off300 * s;
    if (y0) *y0 = pi->vmargin300 * s;
    if (w)  *w  = pi->logical_w300 * s;
    if (h)  *h  = (pi->phys_h300 - 2 * pi->vmargin300) * s;
}

const paper_info_t *paper_by_id(paper_t p)
{
    int n = (int)(sizeof papers / sizeof papers[0]);
    if ((int)p < 0 || (int)p >= n) p = PAPER_A4;
    return &papers[p];
}

const paper_info_t *paper_lookup(const char *name)
{
    for (size_t i = 0; i < sizeof papers / sizeof papers[0]; i++)
        if (strcasecmp(papers[i].name, name) == 0) return &papers[i];
    return NULL;
}

int paper_id_lookup(const char *name)
{
    for (size_t i = 0; i < sizeof papers / sizeof papers[0]; i++)
        if (strcasecmp(papers[i].name, name) == 0) return (int)i;
    return -1;
}

/* ---------------------------------------------------------------- bitmap */

bitmap_t *bitmap_new(int width, int height, int xdpi, int ydpi)
{
    if (width <= 0 || height <= 0) die("bad bitmap geometry %dx%d", width, height);
    bitmap_t *bm = xcalloc(1, sizeof *bm);
    bm->width  = width;
    bm->height = height;
    bm->stride = (width + 7) / 8;
    bm->xdpi   = xdpi;
    bm->ydpi   = ydpi;
    bm->bits   = xcalloc((size_t)bm->stride * height, 1);
    return bm;
}

void bitmap_free(bitmap_t *bm)
{
    if (!bm) return;
    free(bm->bits);
    free(bm);
}

int bitmap_row_blank(const bitmap_t *bm, int y)
{
    const uint8_t *row = bm->bits + (size_t)y * bm->stride;
    for (int i = 0; i < bm->stride; i++)
        if (row[i]) return 0;
    return 1;
}

greymap_t *greymap_new(int w, int h, int xdpi, int ydpi)
{
    if (w <= 0 || h <= 0) die("bad greymap geometry %dx%d", w, h);
    greymap_t *g = xcalloc(1, sizeof *g);
    g->width  = w;
    g->height = h;
    g->xdpi   = xdpi;
    g->ydpi   = ydpi;
    g->grey   = xmalloc((size_t)w * h);
    memset(g->grey, 0xff, (size_t)w * h);   /* white */
    return g;
}

void greymap_free(greymap_t *g)
{
    if (!g) return;
    free(g->grey);
    free(g);
}

/* -------------------------------------------------------------- jobopts */

void jobopt_defaults(jobopt_t *o)
{
    memset(o, 0, sizeof *o);
    o->lang        = LANG_PCL5;
    o->paper       = PAPER_A4;
    o->xdpi        = 600;
    o->ydpi        = 600;
    o->copies      = 1;
    o->economode   = 0;
    o->density     = 0;
    o->pjl         = 1;
    o->duplex      = 0;
    o->compression = -1;     /* per row, whichever of packbits/delta is smaller */
    o->offset_x    = 0;
    o->offset_y    = 0;
    o->xl_major    = 2;
    o->xl_minor    = 0;
    o->ret         = NULL;
    o->pageprotect = NULL;
    o->jobname     = "ml1250";
}
