/* main.c - command line front end. */

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "ml1250.h"

static const char *USAGE =
"ml1250 - printing toolkit for the Samsung ML-1250 and its ML-1200-series siblings\n"
"\n"
"usage:\n"
"  ml1250 list                          list USB printers and their IEEE-1284 IDs\n"
"  ml1250 probe [target]                identify a printer and report what it speaks\n"
"  ml1250 send FILE [target]            send a file byte-for-byte to the printer\n"
"  ml1250 print FILE... [opts] [target] render images and print them\n"
"  ml1250 testpage [opts] [target]      print a diagnostic page\n"
"  ml1250 testprint [what] [target]     ask the printer for one of ITS own\n"
"                                       pages: config, demo or pclfont\n"
"\n"
"input formats for print: .pbm .pgm .ppm (Netpbm, raw or ASCII) and PWG Raster /\n"
"Apple Raster (.pwg, .urf) as produced by Windows' IPP class driver.\n"
"\n"
"target (pick one):\n"
"  -o, --out FILE       write the job to a file (\"-\" for stdout)\n"
"      --usb [N]        Nth USB printer, default 0\n"
"      --dev PATH       explicit device path, e.g. \\\\.\\LPT1\n"
"      --lpt N          parallel port N (1-3)\n"
"      --queue NAME     an existing Windows print queue, sent as RAW\n"
"\n"
"job options:\n"
"      --lang L         pcl5 (default), pclxl, smartgdi\n"
"      --paper P        a4 (default), letter, legal, executive, a5, b5, folio\n"
"      --res N          600 (default) or 300\n"
"      --copies N       default 1\n"
"      --density N      1-5, printer toner density\n"
"      --economode      toner save\n"
"      --compress C     auto (default, best of packbits/delta per row),\n"
"                       0 none, 2 TIFF packbits, 3 delta row\n"
"      --halftone H     cluster (default), bayer, fs, threshold\n"
"      --offset-x N     shift the image N dots right (negative = left)\n"
"      --offset-y N     shift the image N dots down, for margin calibration\n"
"      --ret MODE       Samsung SRT smoothing: normal, enhance, gray, gray+\n"
"      --pageprotect P  auto/on/off; not sent at all by default, because a\n"
"                       stock 4 MB printer has no room for a page buffer\n"
"      --xl-class C     PCL-XL protocol class: 2.0 (default) or 1.1\n"
"      --no-pjl         omit the PJL wrapper\n"
"      --text           testpage only: use the printer's own fonts, no raster\n"
"      --pjl            probe only: also question the printer over PJL and\n"
"                       print whatever it answers\n"
"  -v, --verbose\n"
"\n"
"Why PCL5e: the ML-1250 carries a PCL controller board (the ML-1210/ML-1220M\n"
"carry the SmartGDI one) with 4 MB of RAM.  Modern PCL6/PCL-XL drivers assume a\n"
"far newer interpreter and much more memory, which is why they fail on it.\n";

/* --------------------------------------------------------------- target */

typedef struct {
    enum { T_NONE, T_FILE, T_USB, T_PATH, T_QUEUE } kind;
    const char *arg;
    int         index;
} target_t;

static sink_t *target_open(const target_t *t)
{
    switch (t->kind) {
    case T_FILE:  return sink_open_file(t->arg);
    case T_USB:   return sink_open_usb(t->index);
    case T_PATH:  return sink_open_path(t->arg);
    case T_QUEUE: return sink_open_queue(t->arg);
    default:
        die("no destination given: use --out FILE, --usb, --dev, --lpt or --queue");
    }
    return NULL;
}

/* ----------------------------------------------------------------- list */

static int cmd_list(void)
{
    usbdev_t list[ML_MAX_DEVICES];
    int n = usb_enumerate(list, ML_MAX_DEVICES);
    if (n < 0) die("cannot enumerate USB printers");
    if (n == 0) {
        printf("No USB printers present.\n\n"
               "If the printer is on and connected, Windows should have bound the in-box\n"
               "usbprint.sys to it automatically - no vendor driver needed.  Check Device\n"
               "Manager for an unknown device.  A parallel connection will not appear here;\n"
               "use --lpt 1 instead.\n");
        return 1;
    }
    for (int i = 0; i < n; i++) {
        printf("[%d] %s\n", i, list[i].mdl[0] ? list[i].mdl : "(unnamed)");
        if (list[i].mfg[0]) printf("     manufacturer : %s\n", list[i].mfg);
        if (list[i].cmd[0]) printf("     command sets : %s\n", list[i].cmd);
        if (verbose) {
            printf("     path         : %s\n", list[i].path);
            if (list[i].id1284[0]) printf("     1284 ID      : %s\n", list[i].id1284);
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- probe */

static int cmd_probe(const target_t *t, int with_pjl)
{
    usbdev_t list[ML_MAX_DEVICES];
    const char *path = NULL;
    char idbuf[1024] = "";

    if (t->kind == T_PATH) {
        path = t->arg;
    } else {
        int n = usb_enumerate(list, ML_MAX_DEVICES);
        if (n <= 0) die("no USB printers found");
        int idx = (t->kind == T_USB) ? t->index : 0;
        if (idx < 0 || idx >= n) die("USB index %d out of range (%d found)", idx, n);
        path = list[idx].path;
    }

    printf("device : %s\n", path);

    /* The device path carries the USB ids; 04e8 is Samsung. */
    if (strstr(path, "vid_04e8") || strstr(path, "VID_04E8")) {
        if (strstr(path, "pid_300e") || strstr(path, "PID_300E"))
            printf("model  : Samsung ML-1250 (04e8:300e) - PCL6 / PCL5e, "
                   "use --lang pcl5\n");
        else if (strstr(path, "pid_300c") || strstr(path, "PID_300C"))
            printf("model  : Samsung ML-1210 (04e8:300c) - host-based SmartGDI, "
                   "use --lang smartgdi\n");
        else
            printf("model  : a Samsung printer (vendor 04e8)\n");
    }

    if (usb_read_1284id(path, idbuf, sizeof idbuf) == 0 && idbuf[0]) {
        printf("1284 ID: %s\n", idbuf);
        int said = 0;
        if (strstr(idbuf, "PCLXL") || strstr(idbuf, "PCL XL") || strstr(idbuf, "PCL6")) {
            printf("  -> speaks PCL-XL (PCL6); --lang pclxl\n"); said = 1;
        }
        if (strstr(idbuf, "PCL5") || strstr(idbuf, "PCL 5")) {
            printf("  -> speaks PCL5e; --lang pcl5 (recommended)\n"); said = 1;
        }
        if (strstr(idbuf, "SMART") || strstr(idbuf, "GDI") || strstr(idbuf, "SPL")) {
            printf("  -> host-based SmartGDI/SPL; --lang smartgdi\n"); said = 1;
        }
        if (strstr(idbuf, "POSTSCRIPT")) {
            printf("  -> claims PostScript\n"); said = 1;
        }
        if (!said) {
            printf("  -> the device ID carries no recognisable CMD: field, so the\n"
                   "     printer is not advertising which languages it speaks.\n"
                   "     Nothing is wrong: old printers often report only MFG and\n"
                   "     MDL.  For an ML-1250 use --lang pcl5.\n");
        }
    } else {
        printf("1284 ID: (the device did not answer IOCTL_USBPRINT_GET_1284_ID)\n");
    }

    if (with_pjl) {
        /* Samsung's own tools use the non-standard "INFO PRINTER = ALL"; the
         * HP-standard queries are unproven on this firmware, so try several
         * and report whatever comes back.  Silence is a normal answer. */
        static const char *queries[] = {
            "@PJL INFO PRINTER = ALL",
            "@PJL INFO ID",
            "@PJL INFO CONFIG",
            "@PJL INFO STATUS",
            "@PJL INFO VARIABLES",
        };
        char reply[8192];
        int any = 0;
        for (size_t q = 0; q < sizeof queries / sizeof queries[0]; q++) {
            int n = usb_pjl_query(path, queries[q], reply, sizeof reply, 2500);
            if (n > 0) {
                any = 1;
                printf("\n--- %s ---\n%s\n", queries[q], reply);
            } else if (verbose) {
                printf("    %-26s no reply\n", queries[q]);
            }
        }
        if (!any)
            printf("\nPJL: the printer answered none of these.  That does not mean PJL\n"
                   "     is unsupported - plenty of printers accept PJL commands and\n"
                   "     never talk back.  Try: ml1250 testprint config --usb\n");
    }

    uint8_t st = 0;
    if (usb_read_status(path, &st) == 0) {
        /* Centronics status byte as reported by usbprint.sys. */
        printf("status : 0x%02x  %s%s%s\n", st,
               (st & 0x08) ? "" : "ERROR ",
               (st & 0x20) ? "PAPER-OUT " : "",
               (st & 0x10) ? "SELECTED " : "OFFLINE ");
    }
    return 0;
}

/* ----------------------------------------------------------------- send */

static int cmd_send(const char *file, const target_t *t)
{
    FILE *fp = fopen(file, "rb");
    if (!fp) die("cannot open %s", file);
    sink_t *s = target_open(t);
    uint8_t buf[65536];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof buf, fp)) > 0) {
        if (sink_write(s, buf, n) != 0) {
            warnf("write failed: %s", sink_error(s));
            rc = 1;
            break;
        }
    }
    fclose(fp);
    if (sink_close(s) != 0 && rc == 0) rc = 1;
    return rc;
}

/* ------------------------------------------------------------- test page */

/* Pure-PCL text page.  This is the first thing to try: it uses nothing but the
 * printer's own fonts, so if it prints, the PCL interpreter is alive and the
 * cable and port are fine, and any later failure is ours. */
static int testpage_text(sink_t *s, const jobopt_t *o)
{
    char b[4096];
    const paper_info_t *pi = paper_by_id(o->paper);
    int n = snprintf(b, sizeof b,
        "\x1b%%-12345X@PJL JOB NAME=\"ml1250 text test\"\r\n"
        "@PJL ENTER LANGUAGE=PCL\r\n"
        "\x1b" "E"
        "\x1b&l%dA"
        "\x1b(s0p10h12v0s0b4099T"          /* Courier, 10 cpi, 12 pt, upright */
        "\r\n"
        "   ml1250 - PCL5e text test page\r\n"
        "   ================================================\r\n"
        "\r\n"
        "   If you are reading this, the printer's PCL interpreter works and\r\n"
        "   the cable, port and byte path are all fine.\r\n"
        "\r\n"
        "   paper     : %s (PCL size code %d)\r\n"
        "   requested : %d dpi\r\n"
        "\r\n"
        "   Next step: ml1250 testpage --usb   (the raster test page)\r\n"
        "\r\n"
        "   ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz 0123456789\r\n"
        "   !\"#$%%&'()*+,-./:;<=>?@[\\]^_`{|}~\r\n"
        "\f"
        "\x1b" "E" "\x1b%%-12345X@PJL EOJ\r\n\x1b%%-12345X",
        pi->pcl_code ? pi->pcl_code : 26, pi->name, pi->pcl_code, o->ydpi);
    if (n <= 0 || n >= (int)sizeof b) die("test page overflow");
    return sink_write(s, b, (size_t)n);
}

static void fill_rect(bitmap_t *bm, int x0, int y0, int w, int h, int on)
{
    for (int y = y0; y < y0 + h && y < bm->height; y++)
        for (int x = x0; x < x0 + w && x < bm->width; x++) {
            uint8_t *p = &bm->bits[(size_t)y * bm->stride + (x >> 3)];
            uint8_t m = (uint8_t)(0x80u >> (x & 7));
            if (on) *p |= m; else *p &= (uint8_t)~m;
        }
}

static bitmap_t *make_testpage_raster(const jobopt_t *o)
{
    const paper_info_t *pi = paper_by_id(o->paper);
    int w, h;
    paper_printable(pi, o->xdpi, NULL, NULL, &w, &h);
    w &= ~7;

    /* Build a greymap so the chosen halftone is what is actually exercised. */
    greymap_t *g = greymap_new(w, h, o->xdpi, o->ydpi);

    /* A 1-dot border proves the printable area and shows clipping. */
    bitmap_t *frame = bitmap_new(w, h, o->xdpi, o->ydpi);
    fill_rect(frame, 0, 0, w, 2, 1);
    fill_rect(frame, 0, h - 2, w, 2, 1);
    fill_rect(frame, 0, 0, 2, h, 1);
    fill_rect(frame, w - 2, 0, 2, h, 1);

    /* A row of grey patches: 0%, 10% ... 100%. */
    int patch = w / 12;
    int py = o->ydpi / 2;
    for (int i = 0; i <= 10; i++) {
        int level = 255 - i * 255 / 10;
        for (int y = py; y < py + patch && y < h; y++)
            for (int x = patch / 2 + i * patch; x < patch / 2 + (i + 1) * patch && x < w; x++)
                g->grey[(size_t)y * w + x] = (uint8_t)level;
    }

    /* A vertical gradient below it, to show banding and toner response. */
    int gy = py + patch + o->ydpi / 4;
    int gh = o->ydpi;
    for (int y = gy; y < gy + gh && y < h; y++) {
        int level = 255 - (y - gy) * 255 / (gh ? gh : 1);
        for (int x = patch / 2; x < w - patch / 2; x++)
            g->grey[(size_t)y * w + x] = (uint8_t)level;
    }

    bitmap_t *bm = halftone_apply(g, HT_CLUSTER);
    greymap_free(g);

    /* Overlay the frame and a fine line pattern that exposes resolution. */
    for (size_t i = 0; i < (size_t)bm->stride * bm->height; i++)
        bm->bits[i] |= frame->bits[i];
    bitmap_free(frame);

    int ly = gy + gh + o->ydpi / 4;
    for (int k = 1; k <= 8 && ly + 40 < h; k++) {
        for (int x = patch / 2; x < w - patch / 2; x++)
            if ((x / k) % 2 == 0)
                fill_rect(bm, x, ly, 1, o->ydpi / 6, 1);
        ly += o->ydpi / 6 + o->ydpi / 12;
    }
    return bm;
}

/* ---------------------------------------------------------------- print */

static int emit_job(sink_t *s, bitmap_t **pages, int npages, const jobopt_t *o)
{
    switch (o->lang) {
    case LANG_PCLXL:    return pclxl_write_job(s, pages, npages, o);
    case LANG_SMARTGDI: return smartgdi_write_job(s, pages, npages, o);
    case LANG_PCL5:
    default:            return pcl5_write_job(s, pages, npages, o);
    }
}

static int is_pwg(FILE *fp)
{
    uint8_t m[4];
    long pos = ftell(fp);
    size_t n = fread(m, 1, 4, fp);
    fseek(fp, pos, SEEK_SET);
    if (n != 4) return 0;
    return (m[0] == 'R' && m[1] == 'a' && m[2] == 'S') ||
           (m[1] == 'S' && m[2] == 'a' && m[3] == 'R') ||
           (m[0] == 'U' && m[1] == 'N' && m[2] == 'I' && m[3] == 'R') ||
           (m[0] == 'R' && m[1] == 'I' && m[2] == 'N' && m[3] == 'U');
}

static void clip_warn(const bitmap_t *bm, const jobopt_t *o)
{
    const paper_info_t *pi = paper_by_id(o->paper);
    int maxw, maxh;
    paper_printable(pi, o->xdpi, NULL, NULL, &maxw, &maxh);
    if (bm->width > maxw || bm->height > maxh)
        warnf("image is %dx%d dots but only %dx%d fit on %s at %d dpi; "
              "the printer will clip",
              bm->width, bm->height, maxw, maxh, pi->name, o->xdpi);
}

static int cmd_print(char **files, int nfiles, const jobopt_t *o, halftone_t ht,
                     const target_t *t)
{
    bitmap_t *pages[256];
    int npages = 0;

    for (int i = 0; i < nfiles; i++) {
        FILE *fp = fopen(files[i], "rb");
        if (!fp) die("cannot open %s", files[i]);

        if (is_pwg(fp)) {
            pwg_reader_t *r = pwg_open(fp);
            bitmap_t *bm;
            greymap_t *gm;
            int rc;
            while (npages < (int)(sizeof pages / sizeof pages[0]) &&
                   (rc = pwg_next_page(r, &bm, &gm)) == 1) {
                if (gm) { bm = halftone_apply(gm, ht); greymap_free(gm); }
                if (bm) pages[npages++] = bm;
            }
            if (rc < 0) warnf("%s: %s", files[i], pwg_error(r));
            pwg_close(r);
        } else {
            bitmap_t *bm = NULL;
            greymap_t *gm = NULL;
            if (raster_read_netpbm(fp, o->xdpi, &bm, &gm) != 0)
                die("%s: not a Netpbm or PWG Raster file", files[i]);
            if (gm) { bm = halftone_apply(gm, ht); greymap_free(gm); }
            if (npages < (int)(sizeof pages / sizeof pages[0])) pages[npages++] = bm;
        }
        fclose(fp);
    }

    if (npages == 0) die("no pages to print");
    for (int i = 0; i < npages; i++) clip_warn(pages[i], o);
    if (verbose)
        warnf("%d page(s), %dx%d dots, %s", npages, pages[0]->width, pages[0]->height,
              o->lang == LANG_PCL5 ? "PCL5e" : o->lang == LANG_PCLXL ? "PCL-XL" : "SmartGDI");

    sink_t *s = target_open(t);
    int rc = emit_job(s, pages, npages, o);
    if (rc != 0) warnf("output failed: %s", sink_error(s));
    if (sink_close(s) != 0 && rc == 0) rc = -1;
    for (int i = 0; i < npages; i++) bitmap_free(pages[i]);
    return rc == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------ cli */

static int need_arg(int i, int argc, char **argv)
{
    if (i + 1 >= argc) die("%s needs a value", argv[i]);
    return i + 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fputs(USAGE, stdout); return 2; }

    jobopt_t o;
    jobopt_defaults(&o);


    halftone_t ht = HT_CLUSTER;
    int want_pjl_probe = 0;
    target_t t = { T_NONE, NULL, 0 };
    int text_mode = 0;
    char *files[64];
    int nfiles = 0;

    const char *cmd = argv[1];

    for (int i = 2; i < argc; i++) {
        char *a = argv[i];
        if (strcmp(a, "-o") == 0 || strcmp(a, "--out") == 0) {
            i = need_arg(i, argc, argv); t.kind = T_FILE; t.arg = argv[i];
        } else if (strcmp(a, "--usb") == 0) {
            t.kind = T_USB;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                t.index = atoi(argv[++i]);
        } else if (strcmp(a, "--dev") == 0) {
            i = need_arg(i, argc, argv); t.kind = T_PATH; t.arg = argv[i];
        } else if (strcmp(a, "--lpt") == 0) {
            i = need_arg(i, argc, argv);
            static char lpt[16];
            snprintf(lpt, sizeof lpt, "\\\\.\\LPT%s", argv[i]);
            t.kind = T_PATH; t.arg = lpt;
        } else if (strcmp(a, "--queue") == 0) {
            i = need_arg(i, argc, argv); t.kind = T_QUEUE; t.arg = argv[i];
        } else if (strcmp(a, "--lang") == 0) {
            i = need_arg(i, argc, argv);
            if (strcasecmp(argv[i], "pcl5") == 0)          o.lang = LANG_PCL5;
            else if (strcasecmp(argv[i], "pclxl") == 0 ||
                     strcasecmp(argv[i], "pcl6") == 0)     o.lang = LANG_PCLXL;
            else if (strcasecmp(argv[i], "smartgdi") == 0) o.lang = LANG_SMARTGDI;
            else die("unknown language %s", argv[i]);
        } else if (strcmp(a, "--paper") == 0) {
            i = need_arg(i, argc, argv);
            int p = paper_id_lookup(argv[i]);
            if (p < 0) die("unknown paper size %s", argv[i]);
            o.paper = (paper_t)p;
        } else if (strcmp(a, "--res") == 0) {
            i = need_arg(i, argc, argv);
            o.xdpi = o.ydpi = atoi(argv[i]);
            if (o.xdpi != 300 && o.xdpi != 600)
                die("resolution must be 300 or 600");
        } else if (strcmp(a, "--copies") == 0) {
            i = need_arg(i, argc, argv); o.copies = atoi(argv[i]);
        } else if (strcmp(a, "--density") == 0) {
            i = need_arg(i, argc, argv); o.density = atoi(argv[i]);
        } else if (strcmp(a, "--economode") == 0) {
            o.economode = 1;
        } else if (strcmp(a, "--compress") == 0) {
            i = need_arg(i, argc, argv);
            o.compression = strcasecmp(argv[i], "auto") == 0 ? -1 : atoi(argv[i]);
        } else if (strcmp(a, "--ret") == 0) {
            i = need_arg(i, argc, argv);
            static char retbuf[16];
            for (int k = 0; argv[i][k] && k < 15; k++)
                retbuf[k] = (char)toupper((unsigned char)argv[i][k]);
            o.ret = retbuf;
        } else if (strcmp(a, "--pageprotect") == 0) {
            i = need_arg(i, argc, argv);
            static char ppbuf[16];
            for (int k = 0; argv[i][k] && k < 15; k++)
                ppbuf[k] = (char)toupper((unsigned char)argv[i][k]);
            o.pageprotect = ppbuf;
        } else if (strcmp(a, "--pjl") == 0) {
            want_pjl_probe = 1;
        } else if (strcmp(a, "--xl-class") == 0) {
            i = need_arg(i, argc, argv);
            if (sscanf(argv[i], "%d.%d", &o.xl_major, &o.xl_minor) != 2)
                die("--xl-class wants something like 2.0 or 1.1");
        } else if (strcmp(a, "--offset-x") == 0) {
            i = need_arg(i, argc, argv); o.offset_x = atoi(argv[i]);
        } else if (strcmp(a, "--offset-y") == 0) {
            i = need_arg(i, argc, argv); o.offset_y = atoi(argv[i]);
        } else if (strcmp(a, "--halftone") == 0) {
            i = need_arg(i, argc, argv); ht = halftone_lookup(argv[i]);
        } else if (strcmp(a, "--no-pjl") == 0) {
            o.pjl = 0;
        } else if (strcmp(a, "--text") == 0) {
            text_mode = 1;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            fputs(USAGE, stdout); return 0;
        } else if (a[0] == '-' && a[1]) {
            die("unknown option %s", a);
        } else {
            if (nfiles < (int)(sizeof files / sizeof files[0])) files[nfiles++] = a;
        }
    }

    if (strcmp(cmd, "list") == 0)  return cmd_list();
    if (strcmp(cmd, "probe") == 0) return cmd_probe(&t, want_pjl_probe);
    if (strcmp(cmd, "testprint") == 0) {
        /* Make the printer print one of its OWN pages.  This is pure PJL and
         * carries no page content of ours, so if it comes out, the printer,
         * the cable and the whole byte path are proven good. */
        const char *what = nfiles ? files[0] : "config";
        char kind[16] = "";
        for (int k = 0; what[k] && k < 15; k++)
            kind[k] = (char)toupper((unsigned char)what[k]);
        if (strcmp(kind, "CONFIG") && strcmp(kind, "DEMO") && strcmp(kind, "PCLFONT"))
            die("testprint wants config, demo or pclfont");
        char b[256];
        int n = snprintf(b, sizeof b,
                         "\x1b" "%%-12345X@PJL\r\n"
                         "@PJL TEST PRINT = %s\r\n"
                         "\x1b" "%%-12345X", kind);
        sink_t *s = target_open(&t);
        int rc = sink_write(s, b, (size_t)n);
        if (rc != 0) warnf("output failed: %s", sink_error(s));
        if (sink_close(s) != 0 && rc == 0) rc = -1;
        if (rc == 0)
            printf("Asked the printer for its %s page.\n"
                   "If it comes out, the printer and the whole byte path are\n"
                   "fine, and anything that fails later is on our side.\n", kind);
        return rc == 0 ? 0 : 1;
    }
    if (strcmp(cmd, "send") == 0) {
        if (nfiles != 1) die("send needs exactly one file");
        return cmd_send(files[0], &t);
    }
    if (strcmp(cmd, "print") == 0) {
        if (nfiles == 0) die("print needs at least one input file");
        return cmd_print(files, nfiles, &o, ht, &t);
    }
    if (strcmp(cmd, "testpage") == 0) {
        sink_t *s = target_open(&t);
        int rc;
        if (text_mode) {
            rc = testpage_text(s, &o);
        } else {
            bitmap_t *bm = make_testpage_raster(&o);
            rc = emit_job(s, &bm, 1, &o);
            bitmap_free(bm);
        }
        if (rc != 0) warnf("output failed: %s", sink_error(s));
        if (sink_close(s) != 0 && rc == 0) rc = -1;
        return rc == 0 ? 0 : 1;
    }

    fputs(USAGE, stdout);
    return 2;
}
