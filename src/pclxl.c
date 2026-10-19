/* pclxl.c - PCL-XL (PCL6) Protocol Class 2.0 monochrome raster.
 *
 * Secondary output mode.  PCL5e is preferred for this printer because it is a
 * far smaller, older and more predictable subset; PCL-XL is here because the
 * ML-1250's advertised emulation is "PCL6" and some units may be happier with
 * their native language.
 *
 * Every tag and enumeration below is taken from Microsoft's own shipping
 * PCLXL.GPD (C:\Windows\System32\spool\drivers\x64\3\PCLXL.GPD), which is the
 * authoritative table Windows' in-box PCL6 driver itself uses.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "ml1250.h"

/* Data type tags */
#define T_UBYTE        0xc0
#define T_UINT16       0xc1
#define T_UINT32       0xc2
#define T_UINT16_XY    0xd1
#define T_ATTR_UBYTE   0xf8
#define T_DATALENGTH   0xfa
#define T_DATALEN_BYTE 0xfb

/* Attribute ids */
#define A_COLORSPACE        0x03
#define A_MEDIASIZE         0x25
#define A_MEDIASOURCE       0x26
#define A_ORIENTATION       0x28
#define A_SIMPLEXPAGEMODE   0x34
#define A_PAGECOPIES        0x31
#define A_COLORDEPTH        0x62
#define A_BLOCKHEIGHT       0x63
#define A_COLORMAPPING      0x64
#define A_COMPRESSMODE      0x65
#define A_DESTINATIONSIZE   0x67
#define A_SOURCEHEIGHT      0x6b
#define A_SOURCEWIDTH       0x6c
#define A_STARTLINE         0x6d
#define A_DATAORG           0x82
#define A_MEASURE           0x86
#define A_SOURCETYPE        0x88
#define A_UNITSPERMEASURE   0x89
#define A_ERRORREPORT       0x8f

/* Operator tags */
#define OP_BEGINSESSION     0x41
#define OP_ENDSESSION       0x42
#define OP_BEGINPAGE        0x43
#define OP_ENDPAGE          0x44
#define OP_OPENDATASOURCE   0x48
#define OP_CLOSEDATASOURCE  0x49
#define OP_SETCOLORSPACE    0x6a
#define OP_SETCURSOR        0x6b
#define OP_BEGINIMAGE       0xb0
#define OP_READIMAGE        0xb1
#define OP_ENDIMAGE         0xb2

/* Enumerated values */
#define E_GRAY              0x01   /* ColorSpace                */
#define E_1BIT              0x00   /* ColorDepth                */
#define E_DIRECTPIXEL       0x00   /* ColorMapping              */
#define E_NOCOMPRESSION     0x00
#define E_RLECOMPRESSION    0x01
#define E_INCH              0x00   /* Measure                   */
#define E_DEFAULTSOURCE     0x00   /* SourceType / DataSource   */
#define E_BINARY_LOW_FIRST  0x01   /* DataOrg                   */
#define E_PORTRAIT          0x00
#define E_AUTOSELECT        0x01   /* MediaSource               */
#define E_SIMPLEX_FRONT     0x00
#define E_ERRORPAGE         0x02

/* PCL-XL pads every raster row to a 4 byte boundary. */
#define XL_ROW_PAD 4

typedef struct { sink_t *s; int failed; } out_t;

static void emit(out_t *w, const void *p, size_t n)
{
    if (w->failed) return;
    if (sink_write(w->s, p, n) != 0) w->failed = 1;
}
static void emits(out_t *w, const char *s) { emit(w, s, strlen(s)); }
static void emitb(out_t *w, uint8_t b)     { emit(w, &b, 1); }

/* Attributes are encoded value-first, then the attribute id. */
static void attr_ubyte(out_t *w, uint8_t val, uint8_t id)
{
    uint8_t b[4] = { T_UBYTE, val, T_ATTR_UBYTE, id };
    emit(w, b, 4);
}
static void attr_uint16(out_t *w, unsigned val, uint8_t id)
{
    uint8_t b[5] = { T_UINT16, (uint8_t)(val & 0xff), (uint8_t)(val >> 8),
                     T_ATTR_UBYTE, id };
    emit(w, b, 5);
}
static void attr_uint16_xy(out_t *w, unsigned x, unsigned y, uint8_t id)
{
    uint8_t b[7] = { T_UINT16_XY,
                     (uint8_t)(x & 0xff), (uint8_t)(x >> 8),
                     (uint8_t)(y & 0xff), (uint8_t)(y >> 8),
                     T_ATTR_UBYTE, id };
    emit(w, b, 7);
}
static void embedded(out_t *w, const uint8_t *data, size_t n)
{
    if (n < 256) {
        uint8_t h[2] = { T_DATALEN_BYTE, (uint8_t)n };
        emit(w, h, 2);
    } else {
        uint8_t h[5] = { T_DATALENGTH, (uint8_t)(n & 0xff), (uint8_t)((n >> 8) & 0xff),
                         (uint8_t)((n >> 16) & 0xff), (uint8_t)((n >> 24) & 0xff) };
        emit(w, h, 5);
    }
    emit(w, data, n);
}

/* Rows are sent in blocks so the printer never has to hold the whole image. */
#define XL_BLOCK_ROWS 128

static void write_page(out_t *w, const bitmap_t *bm, const jobopt_t *o,
                       const paper_info_t *pi)
{
    attr_ubyte(w, E_PORTRAIT, A_ORIENTATION);
    attr_ubyte(w, (uint8_t)pi->pclxl_media, A_MEDIASIZE);
    attr_ubyte(w, E_AUTOSELECT, A_MEDIASOURCE);
    attr_ubyte(w, E_SIMPLEX_FRONT, A_SIMPLEXPAGEMODE);
    emitb(w, OP_BEGINPAGE);

    attr_ubyte(w, E_GRAY, A_COLORSPACE);
    emitb(w, OP_SETCOLORSPACE);

    attr_uint16_xy(w, 0, 0, 0x4c /* Point */);
    emitb(w, OP_SETCURSOR);

    /* One unit == one device dot, because UnitsPerMeasure is the resolution. */
    attr_ubyte(w, E_DIRECTPIXEL, A_COLORMAPPING);
    attr_ubyte(w, E_1BIT, A_COLORDEPTH);
    attr_uint16(w, (unsigned)bm->width, A_SOURCEWIDTH);
    attr_uint16(w, (unsigned)bm->height, A_SOURCEHEIGHT);
    attr_uint16_xy(w, (unsigned)bm->width, (unsigned)bm->height, A_DESTINATIONSIZE);
    emitb(w, OP_BEGINIMAGE);

    size_t padded = ((size_t)bm->stride + XL_ROW_PAD - 1) / XL_ROW_PAD * XL_ROW_PAD;
    size_t blockraw = padded * XL_BLOCK_ROWS;
    uint8_t *raw  = xcalloc(blockraw, 1);
    uint8_t *comp = xmalloc(blockraw * 2 + 64);

    for (int y0 = 0; y0 < bm->height && !w->failed; y0 += XL_BLOCK_ROWS) {
        int rows = bm->height - y0;
        if (rows > XL_BLOCK_ROWS) rows = XL_BLOCK_ROWS;

        memset(raw, 0, blockraw);
        for (int r = 0; r < rows; r++)
            memcpy(raw + (size_t)r * padded,
                   bm->bits + (size_t)(y0 + r) * bm->stride, (size_t)bm->stride);

        size_t rawlen = padded * (size_t)rows;
        /* PCL-XL eRLECompression is PackBits, the same coding as PCL5 mode 2. */
        size_t n = pcl_pack_tiff(comp, blockraw * 2 + 64, raw, rawlen);
        int rle = (n != (size_t)-1 && n < rawlen);

        attr_uint16(w, (unsigned)y0, A_STARTLINE);
        attr_uint16(w, (unsigned)rows, A_BLOCKHEIGHT);
        attr_ubyte(w, rle ? E_RLECOMPRESSION : E_NOCOMPRESSION, A_COMPRESSMODE);
        emitb(w, OP_READIMAGE);
        embedded(w, rle ? comp : raw, rle ? n : rawlen);
    }

    free(raw);
    free(comp);

    emitb(w, OP_ENDIMAGE);

    if (o->copies > 1) attr_uint16(w, (unsigned)o->copies, A_PAGECOPIES);
    emitb(w, OP_ENDPAGE);
}

int pclxl_write_job(sink_t *s, bitmap_t **pages, int npages, const jobopt_t *o)
{
    out_t w = { s, 0 };
    const paper_info_t *pi = paper_by_id(o->paper);
    char hdr[256];

    if (o->pjl) {
        emits(&w, "\x1b%-12345X");
        char pjl[512];
        int n = snprintf(pjl, sizeof pjl,
                         "@PJL JOB NAME=\"%s\"\r\n"
                         "@PJL SET RESOLUTION=%d\r\n"
                         "@PJL SET ECONOMODE=%s\r\n",
                         o->jobname ? o->jobname : "ml1250", o->ydpi,
                         o->economode ? "ON" : "OFF");
        if (n > 0 && o->ret)
            n += snprintf(pjl + n, sizeof pjl - (size_t)n,
                          "@PJL SET RET=%s\r\n", o->ret);
        if (n > 0 && o->pageprotect)
            n += snprintf(pjl + n, sizeof pjl - (size_t)n,
                          "@PJL SET PAGEPROTECT=%s\r\n", o->pageprotect);
        if (n > 0)
            n += snprintf(pjl + n, sizeof pjl - (size_t)n,
                          "@PJL ENTER LANGUAGE=PCLXL\r\n");
        if (n > 0) emit(&w, pjl, (size_t)n);
    }

    /* Stream header.  ")" selects the binary, low-byte-first binding.  The
     * protocol class must not exceed what the interpreter implements:
     * Samsung's own ML125.INI for this printer lists "[Protocol Class] 0=1.1
     * 1=2.0", so 2.0 is the ceiling and 1.1 is the safe fallback.
     * It must end with a single newline. */
    int hn = snprintf(hdr, sizeof hdr, ") HP-PCL XL;%d;%d;Comment ml1250\n",
                      o->xl_major, o->xl_minor);
    emit(&w, hdr, (size_t)hn);

    attr_uint16_xy(&w, (unsigned)o->xdpi, (unsigned)o->ydpi, A_UNITSPERMEASURE);
    attr_ubyte(&w, E_INCH, A_MEASURE);
    attr_ubyte(&w, E_ERRORPAGE, A_ERRORREPORT);
    emitb(&w, OP_BEGINSESSION);

    attr_ubyte(&w, E_DEFAULTSOURCE, A_SOURCETYPE);
    attr_ubyte(&w, E_BINARY_LOW_FIRST, A_DATAORG);
    emitb(&w, OP_OPENDATASOURCE);

    for (int i = 0; i < npages && !w.failed; i++)
        write_page(&w, pages[i], o, pi);

    emitb(&w, OP_CLOSEDATASOURCE);
    emitb(&w, OP_ENDSESSION);

    if (o->pjl) {
        emits(&w, "\x1b%-12345X");
        emits(&w, "@PJL EOJ\r\n");
        emits(&w, "\x1b%-12345X");
    }
    return w.failed ? -1 : 0;
}
