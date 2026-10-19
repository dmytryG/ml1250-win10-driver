/* raster.c - input decoders: Netpbm (PBM/PGM/PPM) and PWG Raster / Apple Raster.
 *
 * PWG Raster matters because it is what Windows' in-box "Microsoft IPP Class
 * Driver" POSTs to an IPP Everywhere printer, which is the driverless route
 * for getting modern Windows to talk to this printer at all.
 */

#include <stdlib.h>
#include <string.h>

#include "ml1250.h"

/* ---------------------------------------------------------------- netpbm */

static int pnm_getc(FILE *fp)
{
    int c;
    for (;;) {
        c = fgetc(fp);
        if (c == '#') {                 /* comment to end of line */
            while (c != EOF && c != '\n') c = fgetc(fp);
        }
        return c;
    }
}

static int pnm_int(FILE *fp, int *out)
{
    int c, v = 0, got = 0;
    do {
        c = pnm_getc(fp);
        if (c == EOF) return -1;
    } while (c == ' ' || c == '\t' || c == '\r' || c == '\n');
    while (c >= '0' && c <= '9') {
        v = v * 10 + (c - '0');
        got = 1;
        c = pnm_getc(fp);
    }
    if (!got) return -1;
    *out = v;
    return 0;
}

int raster_read_netpbm(FILE *fp, int dpi, bitmap_t **out_bm, greymap_t **out_gm)
{
    *out_bm = NULL;
    *out_gm = NULL;

    int c1 = fgetc(fp), c2 = fgetc(fp);
    if (c1 != 'P' || c2 < '1' || c2 > '6') return -1;
    int kind = c2 - '0';

    int w, h, maxv = 1;
    if (pnm_int(fp, &w) < 0 || pnm_int(fp, &h) < 0) return -1;
    if (kind != 1 && kind != 4 && pnm_int(fp, &maxv) < 0) return -1;
    if (w <= 0 || h <= 0 || maxv <= 0) return -1;
    /* No extra read here: pnm_int() already consumed the single whitespace
     * character that terminates the last header field, which is exactly the
     * separator the format requires before binary data. */

    if (kind == 4) {                     /* raw PBM: already 1bpp, 1 = black */
        bitmap_t *bm = bitmap_new(w, h, dpi, dpi);
        size_t rowbytes = (size_t)(w + 7) / 8;
        for (int y = 0; y < h; y++) {
            if (fread(bm->bits + (size_t)y * bm->stride, 1, rowbytes, fp) != rowbytes) {
                bitmap_free(bm);
                return -1;
            }
        }
        *out_bm = bm;
        return 0;
    }

    if (kind == 1) {                     /* ASCII PBM */
        bitmap_t *bm = bitmap_new(w, h, dpi, dpi);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int v;
                if (pnm_int(fp, &v) < 0) { bitmap_free(bm); return -1; }
                if (v) bm->bits[(size_t)y * bm->stride + (x >> 3)] |= (uint8_t)(0x80u >> (x & 7));
            }
        *out_bm = bm;
        return 0;
    }

    greymap_t *g = greymap_new(w, h, dpi, dpi);
    int two = maxv > 255;
    for (int y = 0; y < h; y++) {
        uint8_t *dst = g->grey + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            int r, gg, b;
            if (kind == 5 || kind == 2) {
                if (kind == 2) { if (pnm_int(fp, &r) < 0) goto fail; }
                else {
                    int hi = fgetc(fp);
                    if (hi == EOF) goto fail;
                    r = two ? ((hi << 8) | (fgetc(fp) & 0xff)) : hi;
                }
                dst[x] = (uint8_t)(r * 255 / maxv);
            } else {
                if (kind == 3) {
                    if (pnm_int(fp, &r) < 0 || pnm_int(fp, &gg) < 0 || pnm_int(fp, &b) < 0)
                        goto fail;
                } else {
                    /* Binary PPM.  With maxval > 255 each sample is two bytes,
                     * most significant first, interleaved per channel. */
                    int a = fgetc(fp); if (two) a = (a << 8) | (fgetc(fp) & 0xff);
                    int bb = fgetc(fp); if (two) bb = (bb << 8) | (fgetc(fp) & 0xff);
                    int cc = fgetc(fp); if (two) cc = (cc << 8) | (fgetc(fp) & 0xff);
                    if (cc == EOF || a == EOF || bb == EOF) goto fail;
                    r = a; gg = bb; b = cc;
                }
                /* Rec.601 luma, good enough for reprographic work. */
                int lum = (r * 299 + gg * 587 + b * 114) / 1000;
                dst[x] = (uint8_t)(lum * 255 / maxv);
            }
        }
    }
    *out_gm = g;
    return 0;

fail:
    greymap_free(g);
    return -1;
}

/* ------------------------------------------------------------ PWG raster */

/* CUPS/PWG raster sync words. */
#define SYNC_V1      0x52615374u   /* "RaSt" */
#define SYNC_V1_REV  0x74536152u
#define SYNC_V2      0x52615332u   /* "RaS2" - PWG Raster */
#define SYNC_V2_REV  0x32536152u
#define SYNC_V3      0x52615333u   /* "RaS3" */
#define SYNC_V3_REV  0x33536152u
#define SYNC_APPLE   0x554E4952u   /* "UNIR" - Apple Raster */
#define SYNC_APPLE_R 0x52494E55u

/* cups_cspace_e values we care about. */
#define CSPACE_W        0    /* luminance, 0 = black            */
#define CSPACE_RGB      1
#define CSPACE_K        3    /* black,     0 = white, 1 = toner  */
#define CSPACE_SW      18    /* sGray,     0 = black            */
#define CSPACE_SRGB    19
#define CSPACE_ADOBERGB 20

/* v1 header is 420 bytes; v2/v3 add 1376 more for a total of 1796. */
#define HDR_V1_SIZE   420
#define HDR_V2_SIZE  1796

struct pwg_reader {
    FILE    *fp;
    int      swap;          /* stream endianness differs from ours */
    int      version;       /* 1, 2 or 3 */
    char     err[256];
};

const char *pwg_error(const pwg_reader_t *r) { return r ? r->err : "no reader"; }

static uint32_t rd32(const uint8_t *p, int swap)
{
    if (swap) return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

pwg_reader_t *pwg_open(FILE *fp)
{
    pwg_reader_t *r = xcalloc(1, sizeof *r);
    r->fp = fp;

    uint8_t sync[4];
    if (fread(sync, 1, 4, fp) != 4) {
        snprintf(r->err, sizeof r->err, "stream too short for a raster sync word");
        return r;
    }
    uint32_t be = ((uint32_t)sync[0] << 24) | ((uint32_t)sync[1] << 16) |
                  ((uint32_t)sync[2] << 8) | (uint32_t)sync[3];
    switch (be) {
    case SYNC_V1:       r->version = 1; r->swap = 0; break;
    case SYNC_V1_REV:   r->version = 1; r->swap = 1; break;
    case SYNC_V2:       r->version = 2; r->swap = 0; break;
    case SYNC_V2_REV:   r->version = 2; r->swap = 1; break;
    case SYNC_V3:       r->version = 3; r->swap = 0; break;
    case SYNC_V3_REV:   r->version = 3; r->swap = 1; break;
    case SYNC_APPLE:    r->version = 3; r->swap = 0; break;
    case SYNC_APPLE_R:  r->version = 3; r->swap = 1; break;
    default:
        snprintf(r->err, sizeof r->err,
                 "not a PWG/CUPS raster stream (sync word %02x %02x %02x %02x)",
                 sync[0], sync[1], sync[2], sync[3]);
        r->version = 0;
        break;
    }
    return r;
}

void pwg_close(pwg_reader_t *r) { free(r); }

/* Reads one compressed line group.  Returns bytes produced, or -1. */
static int pwg_read_line(pwg_reader_t *r, uint8_t *line, int bytes_per_line, int bpp)
{
    int produced = 0;
    while (produced < bytes_per_line) {
        int n = fgetc(r->fp);
        if (n == EOF) return -1;
        if (n <= 127) {
            /* n+1 copies of the next pixel */
            int reps = n + 1;
            uint8_t pix[8];
            if (bpp > (int)sizeof pix) return -1;
            if ((int)fread(pix, 1, bpp, r->fp) != bpp) return -1;
            for (int i = 0; i < reps && produced + bpp <= bytes_per_line; i++) {
                memcpy(line + produced, pix, bpp);
                produced += bpp;
            }
        } else {
            /* 257-n distinct pixels */
            int cnt = 257 - n;
            int want = cnt * bpp;
            if (produced + want > bytes_per_line) want = bytes_per_line - produced;
            if ((int)fread(line + produced, 1, want, r->fp) != want) return -1;
            produced += want;
        }
    }
    return produced;
}

int pwg_next_page(pwg_reader_t *r, bitmap_t **out_bm, greymap_t **out_gm)
{
    *out_bm = NULL;
    *out_gm = NULL;
    if (!r || r->version == 0) return -1;

    size_t hdrsize = (r->version == 1) ? HDR_V1_SIZE : HDR_V2_SIZE;
    uint8_t *hdr = xmalloc(hdrsize);
    size_t got = fread(hdr, 1, hdrsize, r->fp);
    if (got == 0) { free(hdr); return 0; }              /* clean end of stream */
    if (got != hdrsize) {
        snprintf(r->err, sizeof r->err, "truncated page header (%zu of %zu bytes)",
                 got, hdrsize);
        free(hdr);
        return -1;
    }

    /* Field offsets inside the header: 4 fixed 64-byte strings, then unsigneds. */
    const uint8_t *u = hdr + 256;
    #define U(i) rd32(u + 4 * (i), r->swap)
    uint32_t hwres_x   = U(5);
    uint32_t hwres_y   = U(6);
    uint32_t width     = U(29);
    uint32_t height    = U(30);
    uint32_t bpc       = U(32);   /* cupsBitsPerColor  */
    uint32_t bpp_bits  = U(33);   /* cupsBitsPerPixel  */
    uint32_t bytesline = U(34);   /* cupsBytesPerLine  */
    uint32_t cspace    = U(36);   /* cupsColorSpace    */
    #undef U
    free(hdr);

    if (width == 0 || height == 0 || bytesline == 0 ||
        width > 40000 || height > 40000 || bytesline > 200000) {
        snprintf(r->err, sizeof r->err,
                 "implausible page header: %ux%u, %u bpp, %u bytes/line",
                 width, height, bpp_bits, bytesline);
        return -1;
    }
    if (hwres_x == 0) hwres_x = 600;
    if (hwres_y == 0) hwres_y = 600;

    int bpp = (int)((bpp_bits + 7) / 8);
    if (bpp < 1) bpp = 1;

    uint8_t *line = xmalloc(bytesline);
    bitmap_t  *bm = NULL;
    greymap_t *gm = NULL;

    int one_bit = (bpp_bits == 1);
    if (one_bit) bm = bitmap_new((int)width, (int)height, (int)hwres_x, (int)hwres_y);
    else         gm = greymap_new((int)width, (int)height, (int)hwres_x, (int)hwres_y);

    int y = 0;
    while (y < (int)height) {
        int rep = fgetc(r->fp);
        if (rep == EOF) break;
        if (pwg_read_line(r, line, (int)bytesline, bpp) < 0) {
            snprintf(r->err, sizeof r->err, "truncated raster data at row %d", y);
            free(line);
            bitmap_free(bm);
            greymap_free(gm);
            return -1;
        }
        for (int k = 0; k <= rep && y < (int)height; k++, y++) {
            if (one_bit) {
                size_t n = (size_t)bm->stride < bytesline ? (size_t)bm->stride : bytesline;
                uint8_t *dst = bm->bits + (size_t)y * bm->stride;
                memcpy(dst, line, n);
                /* CSPACE_K: 1 = toner, which is what we want.  W/SW are
                 * luminance, so 0 = black and the plane must be inverted. */
                if (cspace == CSPACE_W || cspace == CSPACE_SW)
                    for (size_t i = 0; i < n; i++) dst[i] = (uint8_t)~dst[i];
            } else {
                uint8_t *dst = gm->grey + (size_t)y * width;
                if (bpp_bits == 8) {
                    for (uint32_t x = 0; x < width && x < bytesline; x++)
                        dst[x] = (cspace == CSPACE_K) ? (uint8_t)(255 - line[x]) : line[x];
                } else if (bpp_bits == 24) {
                    for (uint32_t x = 0; x < width && (x * 3 + 2) < bytesline; x++) {
                        int rr = line[x * 3], gg = line[x * 3 + 1], bb = line[x * 3 + 2];
                        dst[x] = (uint8_t)((rr * 299 + gg * 587 + bb * 114) / 1000);
                    }
                } else if (bpp_bits == 32) {
                    /* CMYK -> grey, crude but only a fallback path. */
                    for (uint32_t x = 0; x < width && (x * 4 + 3) < bytesline; x++) {
                        int c = line[x * 4], m = line[x * 4 + 1];
                        int ye = line[x * 4 + 2], kk = line[x * 4 + 3];
                        int v = 255 - (c > m ? (c > ye ? c : ye) : (m > ye ? m : ye)) - kk;
                        dst[x] = (uint8_t)(v < 0 ? 0 : v);
                    }
                } else {
                    snprintf(r->err, sizeof r->err,
                             "unsupported raster depth: %u bits/pixel, %u bits/colour",
                             bpp_bits, bpc);
                    free(line);
                    greymap_free(gm);
                    return -1;
                }
            }
        }
    }

    free(line);
    *out_bm = bm;
    *out_gm = gm;
    return 1;
}
