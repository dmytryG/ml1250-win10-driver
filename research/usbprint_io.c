/* usbprint_io.c - production-shaped raw I/O to usbprint.sys.
 * clang -municode -O2 -Wall -o usbprint_io.exe usbprint_io.c -lsetupapi
 *
 * Why overlapped: usbprint.sys holds a ReadFile pending until the printer
 * actually produces data, and a WriteFile can block forever on a jammed /
 * offline printer. Synchronous handles give you no way out; FILE_FLAG_OVERLAPPED
 * + WaitForSingleObject + CancelIo do.
 */
#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <winioctl.h>
#include <initguid.h>
#include <setupapi.h>
#include <usbprint.h>
#include <stdio.h>

#define WRITE_TIMEOUT_MS 15000
#define READ_TIMEOUT_MS   3000

/* Open the Nth GUID_DEVINTERFACE_USBPRINT interface. Caller CloseHandle()s. */
static HANDLE open_usbprint(DWORD index, wchar_t *pathOut, size_t pathCch)
{
    HDEVINFO di = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_USBPRINT, NULL, NULL,
                                       DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    SP_DEVICE_INTERFACE_DATA ifd; ifd.cbSize = sizeof(ifd);
    HANDLE h = INVALID_HANDLE_VALUE;

    if (SetupDiEnumDeviceInterfaces(di, NULL, &GUID_DEVINTERFACE_USBPRINT, index, &ifd)) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(di, &ifd, NULL, 0, &need, NULL);
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *det =
            (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)malloc(need);
        if (det) {
            det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W); /* 8 on x64 */
            if (SetupDiGetDeviceInterfaceDetailW(di, &ifd, det, need, NULL, NULL)) {
                if (pathOut) wcsncpy(pathOut, det->DevicePath, pathCch - 1);
                /* SHARED: the spooler and vendor status monitors keep this device
                 * open; an exclusive open just fails with ERROR_ACCESS_DENIED. */
                h = CreateFileW(det->DevicePath,
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, OPEN_EXISTING,
                                FILE_FLAG_OVERLAPPED | FILE_FLAG_WRITE_THROUGH,
                                NULL);
            }
            free(det);
        }
    }
    SetupDiDestroyDeviceInfoList(di);
    return h;
}

/* Overlapped write with a hard deadline. Returns bytes actually accepted. */
static DWORD ov_write(HANDLE h, const void *buf, DWORD len, DWORD timeoutMs)
{
    OVERLAPPED ov = {0};
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return 0;
    DWORD done = 0;
    if (!WriteFile(h, buf, len, &done, &ov)) {
        if (GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, timeoutMs) == WAIT_OBJECT_0) {
                GetOverlappedResult(h, &ov, &done, FALSE);
            } else {
                CancelIo(h);
                GetOverlappedResult(h, &ov, &done, TRUE);   /* bytes that made it */
                fprintf(stderr, "write stalled: %lu/%lu accepted\n",
                        (unsigned long)done, (unsigned long)len);
            }
        } else {
            fprintf(stderr, "WriteFile %lu\n", (unsigned long)GetLastError());
            done = 0;
        }
    }
    CloseHandle(ov.hEvent);
    return done;
}

/* Overlapped read with a deadline; 0 simply means "printer said nothing". */
static DWORD ov_read(HANDLE h, void *buf, DWORD len, DWORD timeoutMs)
{
    OVERLAPPED ov = {0};
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return 0;
    DWORD done = 0;
    if (!ReadFile(h, buf, len, &done, &ov)) {
        if (GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, timeoutMs) == WAIT_OBJECT_0)
                GetOverlappedResult(h, &ov, &done, FALSE);
            else { CancelIo(h); if (!GetOverlappedResult(h, &ov, &done, TRUE)) done = 0; }
        } else done = 0;
    }
    CloseHandle(ov.hEvent);
    return done;
}

int wmain(int argc, wchar_t **argv)
{
    wchar_t path[512] = L"";
    HANDLE h = open_usbprint(0, path, 512);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "no usbprint device (or open failed %lu)\n",
                (unsigned long)GetLastError());
        return 1;
    }
    wprintf(L"opened %ls\n", path);

    /* IOCTLs are METHOD_BUFFERED and complete fast; sync call on an
     * overlapped handle still needs an OVERLAPPED or it can return
     * ERROR_INVALID_PARAMETER, so pass one. */
    {
        OVERLAPPED ov = {0}; ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        BYTE id[1024]; DWORD got = 0;
        BOOL ok = DeviceIoControl(h, IOCTL_USBPRINT_GET_1284_ID, NULL, 0,
                                  id, sizeof(id), &got, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING)
            ok = GetOverlappedResult(h, &ov, &got, TRUE);
        if (ok && got >= 3) {
            unsigned n = ((unsigned)id[0] << 8) | id[1];      /* MSB first */
            if (n > got || n < 14) n = ((unsigned)id[1] << 8) | id[0];
            if (n > got) n = got;
            printf("1284 ID: %.*s\n", (int)(n - 2), (char *)id + 2);
        } else printf("1284 ID failed %lu\n", (unsigned long)GetLastError());
        CloseHandle(ov.hEvent);
    }

    if (argc > 1) {
        HANDLE f = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            static BYTE buf[65536]; DWORD rd; unsigned long long tot = 0;
            while (ReadFile(f, buf, sizeof(buf), &rd, NULL) && rd) {
                DWORD off = 0;
                while (off < rd) {
                    DWORD w = ov_write(h, buf + off, rd - off, WRITE_TIMEOUT_MS);
                    if (!w) { fprintf(stderr, "aborted at %llu\n", tot); goto done; }
                    off += w; tot += w;
                }
            }
        done:
            CloseHandle(f);
            printf("sent %llu bytes\n", tot);
        }
    }

    BYTE back[512];
    DWORD n = ov_read(h, back, sizeof(back), READ_TIMEOUT_MS);
    printf("read back %lu bytes\n", (unsigned long)n);

    CloseHandle(h);
    return 0;
}
