/* infcheck.c - parse an INF with Windows' own SetupAPI and report on the
 * sections a printer minidriver must have.  Non-invasive: it opens and reads
 * the file, it never stages or installs anything. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <stdio.h>
#include <string.h>

static int problems = 0;

static void say(int ok, const char *fmt, ...)
{
    va_list ap;
    if (!ok) problems++;
    fputs(ok ? "  ok   " : "  FAIL ", stdout);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

static int get_field(HINF inf, const char *section, const char *key,
                     int field, char *out, DWORD cap)
{
    INFCONTEXT ctx;
    if (!SetupFindFirstLineA(inf, section, key, &ctx)) return 0;
    return SetupGetStringFieldA(&ctx, field, out, cap, NULL) ? 1 : 0;
}

static int has_section(HINF inf, const char *section)
{
    return SetupGetLineCountA(inf, section) >= 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { puts("usage: infcheck <file.inf>"); return 2; }

    UINT errline = 0;
    HINF inf = SetupOpenInfFileA(argv[1], NULL, INF_STYLE_WIN4, &errline);
    if (inf == INVALID_HANDLE_VALUE) {
        printf("  FAIL SetupOpenInfFile failed at line %u (error %lu)\n",
               errline, (unsigned long)GetLastError());
        return 1;
    }
    printf("parsing %s\n", argv[1]);
    say(1, "INF parses as a Windows 4 style INF");

    char buf[512];
    say(get_field(inf, "Version", "Signature", 1, buf, sizeof buf) &&
        _stricmp(buf, "$Windows NT$") == 0, "[Version] Signature = $Windows NT$");
    say(get_field(inf, "Version", "Class", 1, buf, sizeof buf) &&
        _stricmp(buf, "Printer") == 0, "[Version] Class = Printer");
    say(get_field(inf, "Version", "ClassGUID", 1, buf, sizeof buf) &&
        _stricmp(buf, "{4D36E979-E325-11CE-BFC1-08002BE10318}") == 0,
        "[Version] ClassGUID is the printer class");
    say(get_field(inf, "Version", "CatalogFile", 1, buf, sizeof buf),
        "[Version] CatalogFile present (required: unsigned packages are refused)");
    if (get_field(inf, "Version", "DriverVer", 1, buf, sizeof buf))
        say(1, "[Version] DriverVer = %s", buf);
    else
        say(0, "[Version] DriverVer missing");

    say(has_section(inf, "Manufacturer"), "[Manufacturer] present");
    /* SourceDisksNames may be undecorated or per-architecture, as the in-box
     * printer INFs write it. */
    say(has_section(inf, "SourceDisksNames") ||
        has_section(inf, "SourceDisksNames.amd64") ||
        has_section(inf, "SourceDisksNames.arm64") ||
        has_section(inf, "SourceDisksNames.x86"),
        "[SourceDisksNames] present (plain or architecture-decorated)");
    say(has_section(inf, "SourceDisksFiles"), "[SourceDisksFiles] present");
    say(has_section(inf, "DestinationDirs"), "[DestinationDirs] present");

    /* Walk every model in every decorated manufacturer section. */
    INFCONTEXT mctx;
    int models = 0;
    if (SetupFindFirstLineA(inf, "Manufacturer", NULL, &mctx)) {
        do {
            char base[256] = "";
            if (!SetupGetStringFieldA(&mctx, 1, base, sizeof base, NULL)) continue;

            /* Field 1 is the models-section base name; fields 2..n are the
             * TargetOSVersion decorations. */
            DWORD nfields = SetupGetFieldCount(&mctx);
            for (DWORD d = 1; d <= (nfields ? nfields : 1); d++) {
                char sect[512];
                if (d == 1 && nfields <= 1) snprintf(sect, sizeof sect, "%s", base);
                else if (d == 1)            continue;
                else {
                    char deco[256] = "";
                    if (!SetupGetStringFieldA(&mctx, d, deco, sizeof deco, NULL)) continue;
                    snprintf(sect, sizeof sect, "%s.%s", base, deco);
                }
                if (!has_section(inf, sect)) {
                    say(0, "models section [%s] is referenced but missing", sect);
                    continue;
                }
                INFCONTEXT ictx;
                if (!SetupFindFirstLineA(inf, sect, NULL, &ictx)) continue;
                do {
                    char model[256] = "", install[256] = "";
                    SetupGetStringFieldA(&ictx, 0, model, sizeof model, NULL);
                    if (!SetupGetStringFieldA(&ictx, 1, install, sizeof install, NULL))
                        continue;
                    models++;
                    say(has_section(inf, install),
                        "[%s] \"%s\" -> install section [%s]", sect, model, install);

                    char v[512];
                    say(get_field(inf, install, "DataFile", 1, v, sizeof v),
                        "    [%s] DataFile = %s", install,
                        get_field(inf, install, "DataFile", 1, v, sizeof v) ? v : "?");
                    /* Either the modern CoreDriverSections form that the
                     * in-box printer INFs use, or the older
                     * Include=ntprint.inf + Needs= + DataSection form. */
                    int core = get_field(inf, install, "CoreDriverSections", 1,
                                         v, sizeof v);
                    if (core) {
                        say(strstr(v, "{D20EA372-DD35-4950-9ED8-A6335AFE79F0}") != NULL,
                            "    [%s] CoreDriverSections -> Unidrv core: %s", install, v);
                    } else {
                        char inc[512], nds[512];
                        say(get_field(inf, install, "Include", 1, inc, sizeof inc) &&
                            _stricmp(inc, "NTPRINT.INF") == 0,
                            "    [%s] Include = NTPRINT.INF", install);
                        say(get_field(inf, install, "Needs", 1, nds, sizeof nds),
                            "    [%s] Needs present", install);
                        say(get_field(inf, install, "DataSection", 1, v, sizeof v),
                            "    [%s] DataSection present", install);
                    }
                } while (SetupFindNextLine(&ictx, &ictx));
            }
        } while (SetupFindNextLine(&mctx, &mctx));
    }
    say(models > 0, "%d model entr%s found", models, models == 1 ? "y" : "ies");

    for (const char **a = (const char *[]){ "amd64", "arm64", "x86", NULL }; *a; a++) {
        char sect[128];
        snprintf(sect, sizeof sect, "PrinterPackageInstallation.%s", *a);
        if (!has_section(inf, sect)) continue;
        char v[256];
        say(get_field(inf, sect, "PackageAware", 1, v, sizeof v) &&
            _stricmp(v, "TRUE") == 0, "[%s] PackageAware = TRUE", sect);
        say(get_field(inf, sect, "CoreDriverDependencies", 1, v, sizeof v),
            "[%s] CoreDriverDependencies = %s", sect, v);
    }

    /* Files the package claims to install must exist next to the INF. */
    INFCONTEXT fctx;
    if (SetupFindFirstLineA(inf, "SourceDisksFiles", NULL, &fctx)) {
        char dir[MAX_PATH];
        snprintf(dir, sizeof dir, "%s", argv[1]);
        char *slash = strrchr(dir, '\\');
        if (!slash) slash = strrchr(dir, '/');
        if (slash) *slash = '\0'; else snprintf(dir, sizeof dir, ".");
        do {
            char name[256] = "", path[MAX_PATH];
            SetupGetStringFieldA(&fctx, 0, name, sizeof name, NULL);
            snprintf(path, sizeof path, "%s\\%s", dir, name);
            say(GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES,
                "source file present: %s", name);
        } while (SetupFindNextLine(&fctx, &fctx));
    }

    SetupCloseInfFile(inf);
    printf("\n%s (%d problem%s)\n", problems ? "PROBLEMS FOUND" : "INF looks well formed",
           problems, problems == 1 ? "" : "s");
    return problems ? 1 : 0;
}
