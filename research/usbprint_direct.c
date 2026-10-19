/* usbprint_direct.c - talk to usbprint.sys directly, no spooler, no custom driver.
 * Build (llvm-mingw, x86_64-w64-windows-gnu):
 *   clang -municode -O2 -o usbprint_direct.exe usbprint_direct.c -lsetupapi
 * Usage:
 *   usbprint_direct.exe            -> list USBPRINT interfaces + 1284 ID + status
 *   usbprint_direct.exe <file.prn> -> send raw bytes of file.prn to the first one
 */
#define WIN32_LEAN_AND_MEAN
#define INITGUID                 /* make DEFINE_GUID emit storage, not extern */
#include <windows.h>
#include <winioctl.h>            /* CTL_CODE, FILE_DEVICE_UNKNOWN, METHOD_BUFFERED */
#include <initguid.h>
#include <setupapi.h>
#include <usbprint.h>            /* GUID_DEVINTERFACE_USBPRINT + IOCTL_USBPRINT_* */
#include <stdio.h>

/* Not in mingw's usbprint.h (Win8+ additions). Values derived from the same
 * USBPRINT_IOCTL_INDEX base used by the documented five. UNVERIFIED indices. */
#ifndef IOCTL_USBPRINT_GET_PROTOCOL
#define IOCTL_USBPRINT_GET_PROTOCOL \
    CTL_CODE(FILE_DEVICE_UNKNOWN, USBPRINT_IOCTL_INDEX+17, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_USBPRINT_SET_PROTOCOL \
    CTL_CODE(FILE_DEVICE_UNKNOWN, USBPRINT_IOCTL_INDEX+18, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_USBPRINT_GET_INTERFACE_TYPE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, USBPRINT_IOCTL_INDEX+19, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

static void dump_1284(HANDLE h)
{
    /* MS doc: "output buffer length is placed directly into the USB packet.
     * On some USB print devices, using the maximum 65535 bytes can cause the
     * call to fail with error 23. Reduce the buffer to 4094 bytes or less." */
    BYTE buf[1024];
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_USBPRINT_GET_1284_ID,
                         NULL, 0, buf, sizeof(buf), &got, NULL)) {
        printf("    1284 ID: DeviceIoControl failed, GetLastError=%lu\n",
               (unsigned long)GetLastError());
        return;
    }
    if (got < 3) { printf("    1284 ID: short (%lu bytes)\n", (unsigned long)got); return; }
    /* IEEE-1284: 2-byte length prefix, MSB first, length INCLUDES the 2 bytes. */
    unsigned len = ((unsigned)buf[0] << 8) | buf[1];
    if (len > got || len < 14)                      /* vendors that got it backwards */
        len = ((unsigned)buf[1] << 8) | buf[0];
    if (len > got) len = (unsigned)got;
    if (len < 2)  len = 2;
    printf("    1284 ID (%u bytes): %.*s\n", len - 2, (int)(len - 2), (char *)buf + 2);
}

static void dump_status(HANDLE h)
{
    DWORD st = 0, got = 0;
    if (DeviceIoControl(h, IOCTL_USBPRINT_GET_LPT_STATUS, NULL, 0,
                        &st, sizeof(st), &got, NULL)) {
        /* Centronics-style status byte from the USB class GET_PORT_STATUS request:
         * bit5 = Paper Empty, bit4 = Select/Online, bit3 = NotError. */
        printf("    LPT status: 0x%08lX  (PaperEmpty=%d Select=%d NotError=%d)\n",
               (unsigned long)st, !!(st & 0x20), !!(st & 0x10), !!(st & 0x08));
    } else {
        printf("    LPT status: failed, GetLastError=%lu\n",
               (unsigned long)GetLastError());
    }
}

int wmain(int argc, wchar_t **argv)
{
    HDEVINFO di = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_USBPRINT, NULL, NULL,
                                       DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "SetupDiGetClassDevsW failed %lu\n", (unsigned long)GetLastError());
        return 1;
    }

    SP_DEVICE_INTERFACE_DATA ifd;
    ifd.cbSize = sizeof(ifd);
    int found = 0;
    wchar_t firstPath[512] = L"";

    for (DWORD i = 0;
         SetupDiEnumDeviceInterfaces(di, NULL, &GUID_DEVINTERFACE_USBPRINT, i, &ifd);
         ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(di, &ifd, NULL, 0, &need, NULL);
        if (need == 0) continue;

        SP_DEVICE_INTERFACE_DETAIL_DATA_W *det =
            (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)malloc(need);
        if (!det) break;
        /* cbSize is the size of the FIXED part (8 on x64), NOT `need`. */
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        SP_DEVINFO_DATA dev; dev.cbSize = sizeof(dev);
        if (SetupDiGetDeviceInterfaceDetailW(di, &ifd, det, need, NULL, &dev)) {
            printf("[%lu] %ls\n", (unsigned long)i, det->DevicePath);
            if (!*firstPath) wcsncpy(firstPath, det->DevicePath, 511);
            found++;

            wchar_t idbuf[512] = L"";
            if (SetupDiGetDeviceRegistryPropertyW(di, &dev, SPDRP_FRIENDLYNAME, NULL,
                                                  (PBYTE)idbuf, sizeof(idbuf), NULL))
                printf("    FriendlyName: %ls\n", idbuf);

            HANDLE h = CreateFileW(det->DevicePath,
                                   GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL, OPEN_EXISTING, 0, NULL);
            if (h == INVALID_HANDLE_VALUE) {
                printf("    CreateFileW failed %lu (5=ACCESS_DENIED: spooler/other holder)\n",
                       (unsigned long)GetLastError());
            } else {
                dump_1284(h);
                dump_status(h);
                CloseHandle(h);
            }
        }
        free(det);
    }
    SetupDiDestroyDeviceInfoList(di);

    if (!found) { printf("No GUID_DEVINTERFACE_USBPRINT interfaces present.\n"); return 2; }
    if (argc < 2) return 0;

    /* ---- send raw bytes ---- */
    HANDLE f = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "open %ls failed %lu\n", argv[1], (unsigned long)GetLastError());
        return 3;
    }
    HANDLE h = CreateFileW(firstPath, GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "open printer failed %lu\n", (unsigned long)GetLastError());
        CloseHandle(f); return 4;
    }
    BYTE buf[65536];
    DWORD rd, wr; unsigned long long total = 0;
    while (ReadFile(f, buf, sizeof(buf), &rd, NULL) && rd) {
        BYTE *p = buf; DWORD left = rd;
        while (left) {
            if (!WriteFile(h, p, left, &wr, NULL) || wr == 0) {
                fprintf(stderr, "WriteFile failed %lu\n", (unsigned long)GetLastError());
                CloseHandle(h); CloseHandle(f); return 5;
            }
            p += wr; left -= wr; total += wr;
        }
    }
    printf("sent %llu bytes to %ls\n", total, firstPath);
    CloseHandle(h); CloseHandle(f);
    return 0;
}
