/* ml1250 - printing toolkit for the Samsung ML-1250 (PCL6/PCL5e, 600dpi, 4MB)
 *
 * The ML-1250 is NOT a host-based GDI printer.  Per the Samsung ML-1200 series
 * service manual the ML-1210/ML-1220M carry a SmartGDI controller while the
 * ML-1250 carries its own PCL controller board (PBA MAIN-CONTROLLER PCL,
 * JC92-01333A) with "Emulation: PCL6" and 45 scalable fonts.  Its front panel
 * exposes a PCL 5e menu, and the shipped Linux instructions tell users to pick
 * "Samsung Printer PCL 5e Compatible".  So we drive it with plain PCL5e raster.
 *
 * SmartGDI output is kept as a secondary mode for the ML-1210/ML-1220M siblings.
 */
#ifndef ML1250_H
#define ML1250_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ------------------------------------------------------------------ pages */

typedef enum {
    PAPER_A4 = 0,
    PAPER_LETTER,
    PAPER_LEGAL,
    PAPER_EXECUTIVE,
    PAPER_A5,
    PAPER_B5,
    PAPER_FOLIO
} paper_t;

/* Geometry is held in PCL's own terms.  The HP PCL 5 Technical Reference
 * tabulates, per paper size at 300 dpi (double every value for 600 dpi):
 * the physical page, the *logical* page - which is narrower, inset equally on
 * both sides - and a 1/6 inch unprintable band at top and bottom.  Raster
 * graphics start at the left edge of the logical page and are clipped to the
 * printable area, so these are the numbers that decide whether output lands
 * on the paper. */
typedef struct {
    const char *name;
    double      width_in;
    double      height_in;
    int         pcl_code;      /* <Esc>&l#A; 0 = leave the printer default */
    int         pclxl_media;   /* eLetterPaper etc. */
    int         phys_w300;     /* physical page width,  dots at 300 dpi */
    int         phys_h300;     /* physical page height, dots at 300 dpi */
    int         logical_w300;  /* logical page width = printable width  */
    int         left_off300;   /* logical page inset from the left edge */
    int         vmargin300;    /* unprintable band at top and at bottom */
} paper_info_t;

const paper_info_t *paper_lookup(const char *name);
const paper_info_t *paper_by_id(paper_t p);
int                 paper_id_lookup(const char *name);   /* -1 if unknown */

/* Printable area in dots at the given resolution.  x0/y0 are measured from
 * the top-left corner of the physical sheet. */
void paper_printable(const paper_info_t *pi, int dpi,
                     int *x0, int *y0, int *w, int *h);

/* A monochrome page.  Row-major, 1 bit per pixel, MSB = leftmost pixel,
 * 1 = black (toner).  Rows are padded to whole bytes. */
typedef struct {
    int      width;         /* pixels */
    int      height;        /* pixels */
    int      stride;        /* bytes per row */
    int      xdpi, ydpi;
    uint8_t *bits;
} bitmap_t;

bitmap_t *bitmap_new(int width, int height, int xdpi, int ydpi);
void      bitmap_free(bitmap_t *bm);
/* Returns 1 if the row is entirely white. */
int       bitmap_row_blank(const bitmap_t *bm, int y);

/* ------------------------------------------------------------- greymaps */

typedef struct {
    int      width, height;
    int      xdpi, ydpi;
    uint8_t *grey;          /* 8bpp, 0 = black, 255 = white */
} greymap_t;

greymap_t *greymap_new(int w, int h, int xdpi, int ydpi);
void       greymap_free(greymap_t *g);

typedef enum {
    HT_CLUSTER = 0,   /* 8x8 clustered dot - compresses best, safest for 4MB */
    HT_BAYER,         /* 8x8 ordered dispersed */
    HT_FLOYD,         /* error diffusion - best looking, worst compression */
    HT_THRESHOLD      /* pure 50% threshold - for text-only originals */
} halftone_t;

halftone_t halftone_lookup(const char *name);
bitmap_t  *halftone_apply(const greymap_t *g, halftone_t mode);

/* --------------------------------------------------------------- readers */

/* Netpbm: P4 (raw PBM) -> bitmap, P5 (raw PGM) / P2 / P6 / P3 -> greymap. */
int raster_read_netpbm(FILE *fp, int dpi, bitmap_t **out_bm, greymap_t **out_gm);
/* PWG Raster / Apple Raster stream: yields one page at a time. */
typedef struct pwg_reader pwg_reader_t;
pwg_reader_t *pwg_open(FILE *fp);
/* Returns 1 on page read, 0 at end of stream, -1 on error. */
int           pwg_next_page(pwg_reader_t *r, bitmap_t **out_bm, greymap_t **out_gm);
void          pwg_close(pwg_reader_t *r);
const char   *pwg_error(const pwg_reader_t *r);

/* ---------------------------------------------------------------- output */

/* A sink is anything we can push bytes at: a file, a USB printer, LPT1 or a
 * Windows print queue opened in RAW mode. */
typedef struct sink sink_t;

sink_t *sink_open_file(const char *path);        /* "-" = stdout */
sink_t *sink_open_usb(int index);                /* index into device list */
sink_t *sink_open_path(const char *devpath);     /* \?\usb#... or \.\LPT1 */
sink_t *sink_open_queue(const char *printername);/* spooler, RAW datatype */
int     sink_write(sink_t *s, const void *buf, size_t len);
int     sink_close(sink_t *s);
const char *sink_error(sink_t *s);

/* ------------------------------------------------------------- discovery */

#define ML_MAX_DEVICES 16

typedef struct {
    char path[512];      /* device interface path for CreateFile */
    char id1284[1024];   /* IEEE-1284 device ID, or "" */
    char mfg[128];
    char mdl[128];
    char cmd[256];
} usbdev_t;

int  usb_enumerate(usbdev_t *list, int max);
/* Reads the 1284 device ID from an already-open handle-backed sink. */
int  usb_read_1284id(const char *path, char *buf, size_t buflen);
int  usb_read_status(const char *path, uint8_t *status);
/* Sends a PJL command and returns whatever the printer replies, if anything.
 * Silence is normal; returns bytes collected, or -1 if the device could not
 * be opened or written. */
int  usb_pjl_query(const char *path, const char *cmd, char *buf, size_t cap,
                   int timeout_ms);

/* --------------------------------------------------------------- job opts */

typedef enum { LANG_PCL5 = 0, LANG_PCLXL, LANG_SMARTGDI } lang_t;

typedef struct {
    lang_t      lang;
    paper_t     paper;
    int         xdpi, ydpi;
    int         copies;
    int         economode;      /* PJL SET ECONOMODE */
    int         density;        /* 1..5, PJL SET DENSITY, 0 = leave alone */
    int         pjl;            /* wrap in PJL/UEL */
    int         duplex;         /* unsupported by the ML-1250, kept for siblings */
    int         compression;    /* PCL5: -1 auto, 0 none, 2 packbits, 3 delta */
    int         offset_x;       /* calibration nudge, dots */
    int         offset_y;
    int         xl_major;       /* PCL-XL protocol class; the ML-1250 */
    int         xl_minor;       /* advertises 1.1 and 2.0             */
    const char *ret;            /* @PJL SET RET, Samsung's "SRT": NORMAL,
                                 * ENHANCE, GRAY, GRAY+.  NULL = leave alone */
    const char *pageprotect;    /* @PJL SET PAGEPROTECT; NULL = do not send,
                                 * which is what Samsung's own driver does */
    const char *jobname;
} jobopt_t;

void jobopt_defaults(jobopt_t *o);

/* Encoders.  Each writes a complete, self-contained job. */
int pcl5_write_job(sink_t *s, bitmap_t **pages, int npages, const jobopt_t *o);
int pclxl_write_job(sink_t *s, bitmap_t **pages, int npages, const jobopt_t *o);
int smartgdi_write_job(sink_t *s, bitmap_t **pages, int npages, const jobopt_t *o);

/* Compression primitives (exposed for tests). */
size_t pcl_pack_tiff(uint8_t *dst, size_t dstcap, const uint8_t *src, size_t n);
size_t pcl_pack_delta(uint8_t *dst, size_t dstcap, const uint8_t *cur,
                      const uint8_t *prev, size_t n);
size_t gdi_pack_moditiff(uint8_t *dst, size_t dstcap, const uint8_t *src, size_t n);

/* ----------------------------------------------------------------- misc */

void  die(const char *fmt, ...);
void  warnf(const char *fmt, ...);
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);

extern int verbose;

#endif /* ML1250_H */
