/* device.c - byte transports: file, USB printer, LPT, Windows print queue. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <setupapi.h>
#include <winspool.h>
#include <winioctl.h>
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ml1250.h"

/* usbprint.sys device interface.  Windows binds usbprint.sys to every device
 * matching the USB printer-class compatible IDs
 * (USB\Class_07&SubClass_01&Prot_01/02/03) with no vendor driver at all, so
 * this works on a bare Windows 10/11 install. */
DEFINE_GUID(GUID_DEVINTERFACE_USBPRINT,
            0x28d78fad, 0x5a12, 0x11d1, 0xae, 0x5b, 0x00, 0x00, 0xf8, 0x03, 0xa8, 0xc2);

/* ddk/usbprint.h:
 *   CTL_CODE(FILE_DEVICE_UNKNOWN=0x22, index, METHOD_BUFFERED=0, FILE_ANY_ACCESS=0) */
#define IOCTL_USBPRINT_GET_LPT_STATUS CTL_CODE(FILE_DEVICE_UNKNOWN, 12, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_USBPRINT_GET_1284_ID    CTL_CODE(FILE_DEVICE_UNKNOWN, 13, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_USBPRINT_SOFT_RESET     CTL_CODE(FILE_DEVICE_UNKNOWN, 16, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* ------------------------------------------------------------------ sink */

typedef enum { SK_FILE, SK_HANDLE, SK_QUEUE } sinkkind_t;

struct sink {
    sinkkind_t kind;
    FILE      *fp;         /* SK_FILE */
    int        fp_is_std;
    HANDLE     h;          /* SK_HANDLE */
    HANDLE     hprn;       /* SK_QUEUE */
    char       err[512];
};

static void sink_seterr(sink_t *s, const char *what)
{
    DWORD e = GetLastError();
    char *msg = NULL;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e,
                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&msg, 0, NULL);
    snprintf(s->err, sizeof s->err, "%s: error %lu: %s", what, (unsigned long)e,
             msg ? msg : "(no description)");
    if (msg) LocalFree(msg);
    /* FormatMessage likes to append CRLF */
    size_t n = strlen(s->err);
    while (n && (s->err[n - 1] == '\n' || s->err[n - 1] == '\r'))
        s->err[--n] = '\0';
}

const char *sink_error(sink_t *s) { return s ? s->err : "no sink"; }

sink_t *sink_open_file(const char *path)
{
    sink_t *s = xcalloc(1, sizeof *s);
    s->kind = SK_FILE;
    s->h = INVALID_HANDLE_VALUE;
    if (strcmp(path, "-") == 0) {
        s->fp = stdout;
        s->fp_is_std = 1;
        _setmode(_fileno(stdout), _O_BINARY);
    } else {
        s->fp = fopen(path, "wb");
        if (!s->fp)
            snprintf(s->err, sizeof s->err, "cannot open %s for writing", path);
    }
    return s;
}

sink_t *sink_open_path(const char *devpath)
{
    sink_t *s = xcalloc(1, sizeof *s);
    s->kind = SK_HANDLE;
    s->h = CreateFileA(devpath, GENERIC_WRITE | GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, 0, NULL);
    if (s->h == INVALID_HANDLE_VALUE) {
        /* Some stacks refuse GENERIC_READ; retry write-only. */
        s->h = CreateFileA(devpath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    }
    if (s->h == INVALID_HANDLE_VALUE)
        sink_seterr(s, devpath);
    return s;
}

static sink_t *sink_fail(const char *fmt, const char *arg, int num)
{
    sink_t *s = xcalloc(1, sizeof *s);
    s->kind = SK_HANDLE;
    s->h = INVALID_HANDLE_VALUE;
    if (arg) snprintf(s->err, sizeof s->err, fmt, arg);
    else     snprintf(s->err, sizeof s->err, fmt, num);
    return s;
}

sink_t *sink_open_usb(int index)
{
    usbdev_t list[ML_MAX_DEVICES];
    int n = usb_enumerate(list, ML_MAX_DEVICES);
    if (n <= 0)
        return sink_fail("no USB printers found "
                         "(printer powered on, cable in, and not claimed by another app?)%s",
                         "", 0);
    if (index < 0 || index >= n)
        return sink_fail("USB device index out of range: %d device(s) present", NULL, n);
    return sink_open_path(list[index].path);
}

sink_t *sink_open_queue(const char *printername)
{
    sink_t *s = xcalloc(1, sizeof *s);
    s->kind = SK_QUEUE;
    s->h = INVALID_HANDLE_VALUE;
    if (!OpenPrinterA((LPSTR)printername, &s->hprn, NULL)) {
        sink_seterr(s, printername);
        s->hprn = NULL;
        return s;
    }
    DOC_INFO_1A di;
    di.pDocName    = (LPSTR)"ml1250 raw job";
    di.pOutputFile = NULL;
    di.pDatatype   = (LPSTR)"RAW";
    if (StartDocPrinterA(s->hprn, 1, (LPBYTE)&di) == 0) {
        sink_seterr(s, "StartDocPrinter");
        ClosePrinter(s->hprn);
        s->hprn = NULL;
        return s;
    }
    if (!StartPagePrinter(s->hprn)) {
        sink_seterr(s, "StartPagePrinter");
        EndDocPrinter(s->hprn);
        ClosePrinter(s->hprn);
        s->hprn = NULL;
        return s;
    }
    return s;
}

int sink_write(sink_t *s, const void *buf, size_t len)
{
    if (!s) return -1;
    const uint8_t *p = buf;
    switch (s->kind) {
    case SK_FILE:
        if (!s->fp) return -1;
        return fwrite(p, 1, len, s->fp) == len ? 0 : -1;

    case SK_HANDLE:
        if (s->h == INVALID_HANDLE_VALUE) return -1;
        /* USB 1.1 bulk at 12 Mbit/s: write in modest chunks so a jammed or
         * offline printer surfaces as an error instead of one huge blocking I/O. */
        while (len) {
            DWORD chunk = (DWORD)(len > 16384 ? 16384 : len), done = 0;
            if (!WriteFile(s->h, p, chunk, &done, NULL)) {
                sink_seterr(s, "WriteFile");
                return -1;
            }
            if (done == 0) {
                snprintf(s->err, sizeof s->err, "device accepted 0 bytes");
                return -1;
            }
            p   += done;
            len -= done;
        }
        return 0;

    case SK_QUEUE: {
        if (!s->hprn) return -1;
        DWORD done = 0;
        if (!WritePrinter(s->hprn, (LPVOID)p, (DWORD)len, &done) || done != len) {
            sink_seterr(s, "WritePrinter");
            return -1;
        }
        return 0;
    }
    }
    return -1;
}

int sink_close(sink_t *s)
{
    int rc = 0;
    if (!s) return -1;
    switch (s->kind) {
    case SK_FILE:
        if (s->fp) {
            if (fflush(s->fp) != 0) rc = -1;
            if (!s->fp_is_std) fclose(s->fp);
        } else rc = -1;
        break;
    case SK_HANDLE:
        if (s->h != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(s->h);
            CloseHandle(s->h);
        } else rc = -1;
        break;
    case SK_QUEUE:
        if (s->hprn) {
            EndPagePrinter(s->hprn);
            EndDocPrinter(s->hprn);
            ClosePrinter(s->hprn);
        } else rc = -1;
        break;
    }
    free(s);
    return rc;
}

/* ------------------------------------------------------------- discovery */

static void field_copy(char *dst, size_t cap, const char *src, size_t n)
{
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static const char *field_find(const char *id, const char *longkey, const char *shortkey,
                              size_t *len)
{
    const char *s = strstr(id, longkey);
    size_t klen = strlen(longkey);
    if (!s) { s = strstr(id, shortkey); klen = strlen(shortkey); }
    if (!s) return NULL;
    s += klen;
    const char *e = strchr(s, ';');
    *len = e ? (size_t)(e - s) : strlen(s);
    return s;
}

static void parse_1284(usbdev_t *d)
{
    /* e.g. "MFG:Samsung;MDL:ML-1250;CMD:PCL5E,PCLXL,PJL;CLS:PRINTER;" */
    size_t n;
    const char *v;
    if ((v = field_find(d->id1284, "MANUFACTURER:", "MFG:", &n)))
        field_copy(d->mfg, sizeof d->mfg, v, n);
    if ((v = field_find(d->id1284, "MODEL:", "MDL:", &n)))
        field_copy(d->mdl, sizeof d->mdl, v, n);
    if ((v = field_find(d->id1284, "COMMAND SET:", "CMD:", &n)))
        field_copy(d->cmd, sizeof d->cmd, v, n);
}

int usb_read_1284id(const char *path, char *buf, size_t buflen)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE | GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        h = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;

    uint8_t raw[1026];
    DWORD got = 0;
    int rc = -1;
    if (DeviceIoControl(h, IOCTL_USBPRINT_GET_1284_ID, NULL, 0, raw, sizeof raw, &got, NULL)
        && got >= 2) {
        /* IEEE-1284 device ID: two big-endian length bytes that count
         * themselves, then the ASCII key:value;... string. */
        size_t declared = ((size_t)raw[0] << 8) | raw[1];
        size_t n = (declared >= 2 && declared <= got) ? declared - 2 : (size_t)got - 2;
        if (n >= buflen) n = buflen - 1;
        memcpy(buf, raw + 2, n);
        buf[n] = '\0';
        rc = 0;
    }
    CloseHandle(h);
    return rc;
}

int usb_read_status(const char *path, uint8_t *status)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE | GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD got = 0;
    uint8_t st = 0;
    int rc = (DeviceIoControl(h, IOCTL_USBPRINT_GET_LPT_STATUS, NULL, 0, &st, 1, &got, NULL)
              && got == 1) ? 0 : -1;
    if (rc == 0) *status = st;
    CloseHandle(h);
    return rc;
}

/* One overlapped transfer, waited on with a timeout and cancelled if it does
 * not finish.  Returns bytes transferred, or -1. */
static int timed_io(HANDLE h, HANDLE ev, BOOL write, void *p, DWORD len, int ms)
{
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ResetEvent(ev);
    ov.hEvent = ev;
    DWORD moved = 0;
    BOOL ok = write ? WriteFile(h, p, len, &moved, &ov)
                    : ReadFile(h, p, len, &moved, &ov);
    if (!ok) {
        if (GetLastError() != ERROR_IO_PENDING) return -1;
        if (WaitForSingleObject(ev, ms) != WAIT_OBJECT_0) {
            CancelIo(h);
            GetOverlappedResult(h, &ov, &moved, TRUE);
            return -1;
        }
        if (!GetOverlappedResult(h, &ov, &moved, FALSE)) return -1;
    }
    return (int)moved;
}

/* Sends a PJL command and collects whatever the printer says back.
 *
 * Nothing here is guaranteed to work on a given printer: PJL replies are
 * optional, and the ML-1250's firmware is not documented.  So the read is
 * overlapped with a timeout and a silent printer is a normal outcome, not an
 * error.  Returns the number of bytes collected, or -1 if the device could
 * not even be opened or written to. */
int usb_pjl_query(const char *path, const char *cmd, char *buf, size_t cap,
                  int timeout_ms)
{
    static const char uel[] = "\x1b%-12345X";
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;

    HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!ev) { CloseHandle(h); return -1; }

    char out[512];
    int n = snprintf(out, sizeof out, "%s@PJL\r\n%s\r\n", uel, cmd);
    int rc = -1;

    if (n > 0 && timed_io(h, ev, TRUE, out, (DWORD)n, 5000) == n) {
        /* PJL replies end with a form feed.  Read until we see one, the
         * buffer fills, or the printer simply says nothing - silence is a
         * perfectly normal answer and not an error. */
        size_t got = 0;
        while (got + 1 < cap) {
            int nread = timed_io(h, ev, FALSE, buf + got,
                                 (DWORD)(cap - 1 - got), timeout_ms);
            if (nread <= 0) break;
            got += (size_t)nread;
            if (memchr(buf, '\f', got)) break;
        }
        buf[got] = '\0';
        rc = (int)got;
    }

    /* Leave the printer out of PJL mode again. */
    timed_io(h, ev, TRUE, (void *)uel, (DWORD)(sizeof uel - 1), 2000);

    CloseHandle(ev);
    CloseHandle(h);
    return rc;
}

int usb_enumerate(usbdev_t *list, int max)
{
    HDEVINFO di = SetupDiGetClassDevsA(&GUID_DEVINTERFACE_USBPRINT, NULL, NULL,
                                       DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return -1;

    int n = 0;
    SP_DEVICE_INTERFACE_DATA ifd;
    ifd.cbSize = sizeof ifd;
    for (DWORD i = 0; n < max &&
         SetupDiEnumDeviceInterfaces(di, NULL, &GUID_DEVINTERFACE_USBPRINT, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailA(di, &ifd, NULL, 0, &need, NULL);
        if (need == 0) continue;
        SP_DEVICE_INTERFACE_DETAIL_DATA_A *det = xmalloc(need);
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        SP_DEVINFO_DATA dev;
        dev.cbSize = sizeof dev;
        if (SetupDiGetDeviceInterfaceDetailA(di, &ifd, det, need, NULL, &dev)) {
            memset(&list[n], 0, sizeof list[n]);
            snprintf(list[n].path, sizeof list[n].path, "%s", det->DevicePath);
            if (usb_read_1284id(list[n].path, list[n].id1284, sizeof list[n].id1284) == 0)
                parse_1284(&list[n]);
            if (list[n].mdl[0] == '\0') {
                char fn[256] = "";
                DWORD type = 0;
                if (SetupDiGetDeviceRegistryPropertyA(di, &dev, SPDRP_FRIENDLYNAME, &type,
                                                      (PBYTE)fn, sizeof fn, NULL) ||
                    SetupDiGetDeviceRegistryPropertyA(di, &dev, SPDRP_DEVICEDESC, &type,
                                                      (PBYTE)fn, sizeof fn, NULL))
                    snprintf(list[n].mdl, sizeof list[n].mdl, "%s", fn);
            }
            n++;
        }
        free(det);
    }
    SetupDiDestroyDeviceInfoList(di);
    return n;
}
