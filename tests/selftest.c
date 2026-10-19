/* selftest.c - decode what the encoders produce and compare it with the input.
 *
 * There is no printer in the loop here, so this is the only way to know the
 * byte streams are actually right: every compressor gets a matching decoder
 * written independently from the format description, and a full PCL5e job is
 * parsed back into a bitmap and compared pixel for pixel.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/ml1250.h"

static int failures = 0;
static int checks = 0;

static void ok(int cond, const char *fmt, ...)
{
    va_list ap;
    checks++;
    if (!cond) failures++;
    fputs(cond ? "  ok   " : "  FAIL ", stdout);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

/* ------------------------------------------------------- decoders */

/* PCL method 2 / TIFF PackBits. */
static long unpack_tiff(uint8_t *dst, size_t dstcap, const uint8_t *src, size_t n)
{
    size_t i = 0, o = 0;
    while (i < n) {
        int c = src[i++];
        if (c == 128) continue;
        if (c < 128) {
            size_t cnt = (size_t)c + 1;
            if (i + cnt > n || o + cnt > dstcap) return -1;
            memcpy(dst + o, src + i, cnt);
            i += cnt;
            o += cnt;
        } else {
            size_t cnt = 257 - (size_t)c;
            if (i >= n || o + cnt > dstcap) return -1;
            memset(dst + o, src[i++], cnt);
            o += cnt;
        }
    }
    return (long)o;
}

/* PCL method 3 / delta row, applied on top of the seed row already in dst. */
static int unpack_delta(uint8_t *dst, size_t dstlen, const uint8_t *src, size_t n)
{
    size_t i = 0, pos = 0;
    while (i < n) {
        int ctl = src[i++];
        size_t cnt = (size_t)((ctl >> 5) & 7) + 1;
        size_t off = (size_t)(ctl & 0x1f);
        if (off == 31) {
            while (i < n) {
                int e = src[i++];
                off += (size_t)e;
                if (e != 255) break;
            }
        }
        pos += off;
        if (pos + cnt > dstlen || i + cnt > n) return -1;
        memcpy(dst + pos, src + i, cnt);
        i += cnt;
        pos += cnt;
    }
    return 0;
}

/* Samsung SmartGDI modified-TIFF. */
static long unpack_moditiff(uint8_t *dst, size_t dstcap, const uint8_t *src, size_t n)
{
    size_t i = 0, o = 0;
    while (i < n) {
        unsigned c = src[i++];
        if (c < 0x40) {                       /* short literal */
            size_t cnt = c + 1;
            if (i + cnt > n || o + cnt > dstcap) return -1;
            memcpy(dst + o, src + i, cnt);
            i += cnt; o += cnt;
        } else if (c < 0x80) {                /* short repeat */
            size_t cnt = (size_t)((~c) & 0x7f) + 2;
            if (i >= n || o + cnt > dstcap) return -1;
            memset(dst + o, src[i++], cnt);
            o += cnt;
        } else {
            if (i >= n) return -1;
            unsigned v = (c << 8) | src[i++];
            if (c < 0xc0) {                   /* long literal */
                size_t cnt = (size_t)(v & 0x3fff) + 1;
                if (i + cnt > n || o + cnt > dstcap) return -1;
                memcpy(dst + o, src + i, cnt);
                i += cnt; o += cnt;
            } else {                          /* long repeat */
                size_t cnt = (size_t)((~v) & 0x3fff) + 2;
                if (i >= n || o + cnt > dstcap) return -1;
                memset(dst + o, src[i++], cnt);
                o += cnt;
            }
        }
    }
    return (long)o;
}

/* ------------------------------------------------------ test payloads */

static uint32_t rng_state = 12345;
static uint32_t rnd(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

/* Build data with the pathological shapes: all zero, all ones, alternating,
 * long runs, short runs, runs at the boundary of the short/long encodings. */
static void make_pattern(uint8_t *buf, size_t n, int which)
{
    switch (which) {
    case 0: memset(buf, 0x00, n); break;
    case 1: memset(buf, 0xff, n); break;
    case 2: for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)(i & 1 ? 0xff : 0x00); break;
    case 3: for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)rnd(); break;
    case 4: for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)(i / 7); break;
    case 5:
        /* runs whose length lands exactly on the 64 / 128 / 16384 boundaries */
        memset(buf, 0, n);
        for (size_t i = 0, v = 1; i < n; v++) {
            size_t run = (v % 5 == 0) ? 65 : (v % 3 == 0) ? 64 : (v % 7 == 0) ? 130 : 2;
            if (i + run > n) run = n - i;
            memset(buf + i, (uint8_t)v, run);
            i += run;
        }
        break;
    default:
        for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)((rnd() & 7) ? 0 : rnd());
        break;
    }
}

static void test_compressors(void)
{
    const size_t N = 620;          /* one A4 row at 600 dpi */
    uint8_t *in   = malloc(N);
    uint8_t *prev = malloc(N);
    uint8_t *comp = malloc(N * 3 + 64);
    uint8_t *out  = malloc(N * 2);

    puts("compressors");
    for (int p = 0; p < 7; p++) {
        make_pattern(in, N, p);

        size_t n = pcl_pack_tiff(comp, N * 3 + 64, in, N);
        long got = (n == (size_t)-1) ? -1 : unpack_tiff(out, N * 2, comp, n);
        ok(got == (long)N && memcmp(in, out, N) == 0,
           "PCL packbits  pattern %d: %zu -> %zu bytes", p, N, n);

        make_pattern(prev, N, (p + 3) % 7);
        memcpy(out, prev, N);
        n = pcl_pack_delta(comp, N * 3 + 64, in, prev, N);
        int rc = (n == (size_t)-1) ? -1 : unpack_delta(out, N, comp, n);
        ok(rc == 0 && memcmp(in, out, N) == 0,
           "PCL delta row pattern %d: %zu -> %zu bytes", p, N, n);

        n = gdi_pack_moditiff(comp, N * 3 + 64, in, N);
        got = (n == (size_t)-1) ? -1 : unpack_moditiff(out, N * 2, comp, n);
        ok(got == (long)N && memcmp(in, out, N) == 0,
           "SmartGDI mtiff pattern %d: %zu -> %zu bytes", p, N, n);
    }

    /* Identical rows must encode to nothing at all under delta row. */
    make_pattern(in, N, 3);
    size_t n = pcl_pack_delta(comp, N * 3 + 64, in, in, N);
    ok(n == 0, "PCL delta row: unchanged row encodes to 0 bytes (got %zu)", n);

    free(in); free(prev); free(comp); free(out);
}

/* ------------------------------------------- end-to-end PCL5e round trip */

/* A deliberately small, deliberately independent PCL5e raster parser. */
typedef struct {
    const uint8_t *p, *end;
    int width, height, res, mode;
    uint8_t *page;      /* height * stride */
    int stride;
    int y;
} pclparse_t;

static int pcl_num(const uint8_t **p, const uint8_t *end, long *out)
{
    long v = 0;
    int sign = 1, any = 0;
    if (*p < end && (**p == '-' || **p == '+')) { if (**p == '-') sign = -1; (*p)++; }
    while (*p < end && **p >= '0' && **p <= '9') { v = v * 10 + (*(*p)++ - '0'); any = 1; }
    *out = v * sign;
    return any;
}

static int parse_pcl(const uint8_t *buf, size_t len, pclparse_t *st)
{
    const uint8_t *p = buf, *end = buf + len;
    uint8_t *rowbuf = NULL;
    memset(st, 0, sizeof *st);

    while (p < end) {
        if (*p != 0x1b) { p++; continue; }
        p++;
        if (p >= end) break;

        /* two-character sequences: <Esc>E, <Esc>%... */
        if (*p == 'E') { p++; continue; }
        if (*p == '%') { while (p < end && *p != 'X') p++; if (p < end) p++; continue; }

        /* A PCL escape sequence is  ESC <param 0x21-0x2f> <group 0x60-0x7e>
         * then one or more  <value><terminator>  pairs; a lowercase
         * terminator means another pair follows with the same param/group. */
        uint8_t param = *p++;
        if (p >= end) break;
        uint8_t group = *p++;
        for (;;) {
            long v = 0;
            (void)pcl_num(&p, end, &v);
            if (p >= end) break;
            uint8_t term = *p++;
            int is_last = (term >= 'A' && term <= 'Z');
            uint8_t cls = (uint8_t)(is_last ? term : term - 32);

            if (param == '*') {
                if (group == 't' && cls == 'R') st->res = (int)v;
                if (group == 'r' && cls == 'S') st->width = (int)v;
                if (group == 'r' && cls == 'T') st->height = (int)v;
                if (group == 'b' && cls == 'M') st->mode = (int)v;
                if (group == 'r' && cls == 'A') {           /* start raster */
                    st->stride = (st->width + 7) / 8;
                    if (st->height <= 0 || st->stride <= 0) return -1;
                    st->page = calloc((size_t)st->stride * st->height, 1);
                    rowbuf = calloc((size_t)st->stride, 1);
                    st->y = 0;
                }
                if (group == 'b' && cls == 'Y' && st->page) {   /* skip blank rows */
                    st->y += (int)v;
                    memset(rowbuf, 0, (size_t)st->stride);
                }
                if (group == 'b' && cls == 'W' && st->page) {   /* row data */
                    size_t n = (size_t)v;
                    if (p + n > end) return -1;
                    if (st->mode == 2) {
                        if (unpack_tiff(rowbuf, (size_t)st->stride, p, n) < 0) return -1;
                    } else if (st->mode == 3) {
                        if (n && unpack_delta(rowbuf, (size_t)st->stride, p, n) < 0) return -1;
                    } else {
                        memset(rowbuf, 0, (size_t)st->stride);
                        memcpy(rowbuf, p, n < (size_t)st->stride ? n : (size_t)st->stride);
                    }
                    if (st->y < st->height)
                        memcpy(st->page + (size_t)st->y * st->stride, rowbuf, (size_t)st->stride);
                    st->y++;
                    p += n;
                }
                if (group == 'r' && cls == 'C' && st->page) { /* end raster */ }
            }
            if (is_last) break;
        }
    }
    free(rowbuf);
    return st->page ? 0 : -1;
}

static bitmap_t *make_probe_bitmap(int w, int h)
{
    bitmap_t *bm = bitmap_new(w, h, 600, 600);
    for (int y = 0; y < h; y++) {
        uint8_t *row = bm->bits + (size_t)y * bm->stride;
        if (y % 17 == 3) continue;                     /* blank rows to exercise *b#Y */
        for (int x = 0; x < bm->stride; x++) {
            if (y < h / 3)       row[x] = (uint8_t)((x / 5) & 1 ? 0xff : 0x00);
            else if (y < 2*h/3)  row[x] = (uint8_t)rnd();
            else                 row[x] = (uint8_t)(x % 37 == 0 ? 0xf0 : 0x00);
        }
    }
    return bm;
}

static void test_roundtrip(int mode)
{
    const int W = 1200, H = 400;
    bitmap_t *bm = make_probe_bitmap(W, H);

    char tmp[] = "ml1250-selftest.prn";
    jobopt_t o;
    jobopt_defaults(&o);
    o.compression = mode;
    o.paper = PAPER_A4;

    sink_t *s = sink_open_file(tmp);
    int rc = pcl5_write_job(s, &bm, 1, &o);
    sink_close(s);
    ok(rc == 0, "PCL5e job written (compression mode %d)", mode);

    FILE *fp = fopen(tmp, "rb");
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)len);
    if (fread(buf, 1, (size_t)len, fp) != (size_t)len) { puts("  FAIL read back"); failures++; }
    fclose(fp);
    remove(tmp);

    pclparse_t st;
    int prc = parse_pcl(buf, (size_t)len, &st);
    ok(prc == 0, "mode %d: stream parses (%ld bytes)", mode, len);
    if (prc == 0) {
        ok(st.width == W && st.height == H,
           "mode %d: geometry %dx%d matches %dx%d", mode, st.width, st.height, W, H);
        ok(st.res == 600, "mode %d: resolution %d", mode, st.res);
        int diff = 0;
        if (st.stride == bm->stride)
            diff = memcmp(st.page, bm->bits, (size_t)bm->stride * H) != 0;
        else
            diff = 1;
        ok(!diff, "mode %d: decoded page is bit-identical to the source", mode);
        if (diff && st.stride == bm->stride) {
            for (int y = 0; y < H; y++)
                if (memcmp(st.page + (size_t)y * st.stride,
                           bm->bits + (size_t)y * bm->stride, (size_t)st.stride)) {
                    printf("       first differing row: %d\n", y);
                    break;
                }
        }
        double ratio = (double)len / ((double)bm->stride * H);
        printf("       compression: %.1f%% of raw\n", ratio * 100.0);
        free(st.page);
    }
    free(buf);
    bitmap_free(bm);
}

/* ------------------------------------------------------ PWG Raster input */

/* Build a PWG Raster stream by hand and check the reader gets it back.
 * This is the format Windows' IPP class driver emits, so there is no other
 * way to exercise the decoder without a Windows IPP queue in the loop. */

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* Field indices into the unsigned block that starts at offset 256. */
#define F_HWRES_X   5
#define F_HWRES_Y   6
#define F_WIDTH    29
#define F_HEIGHT   30
#define F_BPC      32
#define F_BPP      33
#define F_BPL      34
#define F_CSPACE   36

static void pwg_header(FILE *fp, uint32_t w, uint32_t h, uint32_t bpp,
                       uint32_t bpl, uint32_t cspace)
{
    uint8_t hdr[1796];
    memset(hdr, 0, sizeof hdr);
    uint8_t *u = hdr + 256;
    put_be32(u + 4 * F_HWRES_X, 600);
    put_be32(u + 4 * F_HWRES_Y, 600);
    put_be32(u + 4 * F_WIDTH,   w);
    put_be32(u + 4 * F_HEIGHT,  h);
    put_be32(u + 4 * F_BPC,     bpp == 1 ? 1 : 8);
    put_be32(u + 4 * F_BPP,     bpp);
    put_be32(u + 4 * F_BPL,     bpl);
    put_be32(u + 4 * F_CSPACE,  cspace);
    fwrite(hdr, 1, sizeof hdr, fp);
}

/* Encode one line group: repeat count, then runs over the whole line. */
static void pwg_line(FILE *fp, const uint8_t *line, size_t bpl, int repeat)
{
    fputc(repeat, fp);                 /* line appears repeat+1 times */
    size_t i = 0;
    while (i < bpl) {
        size_t run = 1;
        while (i + run < bpl && line[i + run] == line[i] && run < 128) run++;
        if (run >= 2) {
            fputc((int)(run - 1), fp); /* 0..127 -> count+1 copies */
            fputc(line[i], fp);
            i += run;
        } else {
            size_t start = i, lit = 0;
            while (i < bpl && lit < 128 &&
                   !(i + 1 < bpl && line[i + 1] == line[i])) { i++; lit++; }
            if (lit == 0) { lit = 1; i++; }
            fputc((int)(257 - lit), fp);  /* 128..255 -> 257-n literals */
            fwrite(line + start, 1, lit, fp);
        }
    }
}

static void test_pwg(void)
{
    puts("\nPWG Raster input");
    const uint32_t W = 200, H = 16;

    /* --- black_1: 1 bpp, CUPS_CSPACE_K, 1 means toner --- */
    {
        const char *tmp = "ml1250-selftest.pwg";
        size_t bpl = (W + 7) / 8;
        uint8_t *line = malloc(bpl);
        FILE *fp = fopen(tmp, "wb");
        fwrite("RaS2", 1, 4, fp);
        pwg_header(fp, W, H, 1, (uint32_t)bpl, 3 /* CSPACE_K */);
        for (uint32_t y = 0; y < H; ) {
            memset(line, (y / 4) & 1 ? 0xff : 0x0f, bpl);
            pwg_line(fp, line, bpl, 3);     /* 4 identical rows at a time */
            y += 4;
        }
        fclose(fp);

        fp = fopen(tmp, "rb");
        pwg_reader_t *r = pwg_open(fp);
        bitmap_t *bm = NULL; greymap_t *gm = NULL;
        int rc = pwg_next_page(r, &bm, &gm);
        ok(rc == 1 && bm && !gm, "black_1 page decodes to a 1bpp bitmap");
        if (bm) {
            ok(bm->width == (int)W && bm->height == (int)H,
               "black_1 geometry %dx%d", bm->width, bm->height);
            ok(bm->xdpi == 600 && bm->ydpi == 600, "black_1 resolution 600 dpi");
            int good = 1;
            for (uint32_t y = 0; y < H; y++) {
                uint8_t want = ((y / 4) & 1) ? 0xff : 0x0f;
                if (bm->bits[(size_t)y * bm->stride] != want) { good = 0; break; }
            }
            ok(good, "black_1 pixels survive the line-repeat and run coding");
            /* CSPACE_K: a 1 bit is toner, so it must NOT be inverted. */
            ok(bm->bits[0] == 0x0f, "black_1 plane is not inverted");
            bitmap_free(bm);
        }
        ok(pwg_next_page(r, &bm, &gm) == 0, "clean end of stream after one page");
        pwg_close(r); fclose(fp); remove(tmp); free(line);
    }

    /* --- sgray_8: 8 bpp luminance, 0 = black --- */
    {
        const char *tmp = "ml1250-selftest2.pwg";
        size_t bpl = W;
        uint8_t *line = malloc(bpl);
        FILE *fp = fopen(tmp, "wb");
        fwrite("RaS2", 1, 4, fp);
        pwg_header(fp, W, H, 8, (uint32_t)bpl, 18 /* CSPACE_SW */);
        for (uint32_t y = 0; y < H; y++) {
            for (uint32_t x = 0; x < W; x++) line[x] = (uint8_t)(x & 0xff);
            pwg_line(fp, line, bpl, 0);
        }
        fclose(fp);

        fp = fopen(tmp, "rb");
        pwg_reader_t *r = pwg_open(fp);
        bitmap_t *bm = NULL; greymap_t *gm = NULL;
        int rc = pwg_next_page(r, &bm, &gm);
        ok(rc == 1 && gm && !bm, "sgray_8 page decodes to a greymap");
        if (gm) {
            ok(gm->width == (int)W && gm->height == (int)H,
               "sgray_8 geometry %dx%d", gm->width, gm->height);
            int good = 1;
            for (uint32_t y = 0; y < H && good; y++)
                for (uint32_t x = 0; x < W; x++)
                    if (gm->grey[(size_t)y * W + x] != (uint8_t)(x & 0xff)) { good = 0; break; }
            ok(good, "sgray_8 pixels survive the run coding");
            bitmap_t *ht = halftone_apply(gm, HT_CLUSTER);
            ok(ht && ht->width == (int)W, "halftoning the decoded greymap works");
            bitmap_free(ht);
            greymap_free(gm);
        }
        pwg_close(r); fclose(fp); remove(tmp); free(line);
    }

    /* A stream that is not raster at all must be refused, not misparsed. */
    {
        const char *tmp = "ml1250-selftest3.bin";
        FILE *fp = fopen(tmp, "wb");
        fwrite("%PDF-1.4\n", 1, 9, fp);
        fclose(fp);
        fp = fopen(tmp, "rb");
        pwg_reader_t *r = pwg_open(fp);
        bitmap_t *bm; greymap_t *gm;
        ok(pwg_next_page(r, &bm, &gm) == -1, "a non-raster stream is rejected");
        pwg_close(r); fclose(fp); remove(tmp);
    }
}

int main(void)
{
    test_compressors();
    test_pwg();
    puts("\nPCL5e end to end");
    test_roundtrip(2);
    test_roundtrip(3);
    test_roundtrip(0);
    test_roundtrip(-1);   /* per-row automatic mode selection */

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
