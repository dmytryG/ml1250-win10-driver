/* spool_raw.c - push raw bytes through the Windows print spooler.
 * Build: clang -municode -O2 -o spool_raw.exe spool_raw.c -lwinspool
 * Usage: spool_raw.exe "Queue Name" file.prn
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winspool.h>
#include <stdio.h>

int wmain(int argc, wchar_t **argv)
{
    if (argc < 3) {
        fwprintf(stderr, L"usage: %ls \"Printer Name\" file.prn\n", argv[0]);
        return 1;
    }

    HANDLE hPrinter = NULL;
    /* PRINTER_ACCESS_USE is enough to submit a job; PRINTER_ALL_ACCESS needs admin.
     * OpenPrinter2W with PRINTER_OPTION_NO_CACHE avoids stale spooler client cache. */
    const wchar_t *dt = (argc > 3) ? argv[3] : L"RAW";
    PRINTER_DEFAULTSW pd = { .pDatatype = (LPWSTR)dt,
                             .pDevMode  = NULL,
                             .DesiredAccess = PRINTER_ACCESS_USE };
    if (!OpenPrinterW(argv[1], &hPrinter, &pd)) {
        fwprintf(stderr, L"OpenPrinterW failed %lu\n", (unsigned long)GetLastError());
        return 2;
    }

    DOC_INFO_1W di = { .pDocName    = (LPWSTR)L"ml1250 raw job",
                       .pOutputFile = NULL,
                       .pDatatype   = (LPWSTR)dt };   /* <- the passthrough switch */

    DWORD job = StartDocPrinterW(hPrinter, 1, (LPBYTE)&di);
    if (job == 0) {
        fwprintf(stderr, L"StartDocPrinterW failed %lu\n", (unsigned long)GetLastError());
        ClosePrinter(hPrinter); return 3;
    }
    if (!StartPagePrinter(hPrinter)) {
        fwprintf(stderr, L"StartPagePrinter failed %lu\n", (unsigned long)GetLastError());
        EndDocPrinter(hPrinter); ClosePrinter(hPrinter); return 4;
    }

    HANDLE f = CreateFileW(argv[2], GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        fwprintf(stderr, L"open %ls failed %lu\n", argv[2], (unsigned long)GetLastError());
        EndPagePrinter(hPrinter); EndDocPrinter(hPrinter); ClosePrinter(hPrinter); return 5;
    }

    BYTE buf[65536]; DWORD rd, wr; unsigned long long total = 0; int ok = 1;
    while (ReadFile(f, buf, sizeof(buf), &rd, NULL) && rd) {
        BYTE *p = buf; DWORD left = rd;
        while (left) {
            /* WritePrinter can short-write; loop. */
            if (!WritePrinter(hPrinter, p, left, &wr) || wr == 0) {
                fwprintf(stderr, L"WritePrinter failed %lu\n", (unsigned long)GetLastError());
                ok = 0; break;
            }
            p += wr; left -= wr; total += wr;
        }
        if (!ok) break;
    }
    CloseHandle(f);

    EndPagePrinter(hPrinter);
    EndDocPrinter(hPrinter);
    ClosePrinter(hPrinter);
    if (ok) wprintf(L"job %lu: wrote %llu bytes\n", (unsigned long)job, total);
    return ok ? 0 : 6;
}
