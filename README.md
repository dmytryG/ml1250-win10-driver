# ml1250 — Samsung ML-1250 on Windows 10/11 x64

**English** · [Русский](README_ru.md)

A printer driver for the Samsung ML-1250 on current Windows, plus a
command-line tool for diagnostics and for printing without the Windows
print queue.

> [!WARNING]
> **Vibecoded.** This project was written largely by an AI coding assistant,
> steered by a human: the code, the driver, the research and this text.
> Everything listed under [What has been verified](#what-has-been-verified)
> really was checked, including on a real printer, but the code has not had
> a conventional human review. Use it at your own risk. The installer adds
> a self-signed certificate to the machine's trusted roots, so read
> [About signing](#about-signing--read-this) before running it.

---

## The key fact: the ML-1250 is not a GDI printer

Most of what the internet says about "Samsung lasers of that era" does not
apply to the ML-1250, which is why driver hunting ends in a dead end. The
ML-1200 series service manual (`ML-1210 / ML-1250 / ML-1220M`) has this
table:

| | ML-1210 / ML-1220M | **ML-1250** |
|---|---|---|
| Resolution | 600 × 600 | **1200 × 600** |
| Controller | Jupiter | **separate board** |
| Memory | 8 MB Equivalent | **4 MB** |
| **Emulation** | **SmartGDI** | **PCL6** |

The manual has separate block diagrams for them, "GDI Block Diagram
(ML-1210/ML-1220M)" and "PCL Block Diagram (ML-1250)", and different
controller boards: `PBA MAIN-CONTROLLER PCL, JC92-01333A` is fitted only
to the ML-1250.

The user's manual agrees: `Emulation: PCL6`, `Fonts: 1 bitmap, 45 scalable`
(a real interpreter, not host-based rasterisation on the PC), `RAM: 4 MB`.
The front panel has its own **PCL 5e** menu (Typeface, Symbol Set, Pitch,
Lines Per Page, Point Size, Courier), and the Linux instructions say to
pick **"Samsung Printer PCL 5e Compatible"** in `printtool`.

The decisive evidence is **Samsung's original driver CD** (`ML_1250_250.iso`,
part no. JC46-00127A, 2002; it is on archive.org):

* only a `PCL/` directory — no trace of a GDI/SPL driver;
* `ML125.INI`: `[Protocol Class] 0=1.1 1=2.0` — the PCL-XL protocol classes
  the interpreter implements;
* `ML125.DDF` with the comment `// "Folio": undefined in PCL-XL`;
* `USB/SSUSB.INF`: `USB\VID_04E8&PID_300E ;ML-1250`
  (for comparison, `PID_300C` is the ML-1210);
* `LINUX/SSPRT.DB` — Samsung's own "emulation → Ghostscript device" table.
  The ML-1250 is listed under `SECPCL6 / GSDriver: pxlmono` **and**
  `SECPCL5 / GSDriver: ljet4`, and it is **not** under
  `SECPRT / GSDriver: gdi` ("PrinThru Compatible"), where the genuinely
  host-based models are.

So **Samsung itself prescribes `ljet4`, i.e. PCL5e, for the ML-1250**, and
that is exactly what this driver does.

> The only source saying otherwise is the splix README: "older SPL1-based
> models (**ML-12xx**, ML-14xx) do not work. Use these printers with the
> older "gdi" driver". That is a pattern over `ML-12xx`, not an analysis of
> this model, and it contradicts both the service manual and the vendor's
> CD. The ML-1250 simply has a different controller board.

One more correction to the spec sheet: the `1200 × 600` in the table is the
marketing "1200 dpi class". The engine is **600 × 600** plus SRT edge
smoothing. Generate raster at 600 or 300 dpi, never at 1200.

### Why nothing worked before

1. **There is no native x64 driver.** The service manual lists
   `Windows 95/98/2000/Me/NT`. A 32-bit v3 driver cannot be installed on
   x64 Windows at all — the spooler needs a native binary.
2. **Universal drivers speak too new a PCL.** Samsung UPD, HP UPD and the
   in-box "Microsoft PCL6 Class Driver" emit PCL-XL Protocol Class 3.0. The
   ML-1250 has a year-2000 class 1.1/2.0 interpreter — hence blank pages
   and `PCL XL error, Subsystem: KERNEL`.
3. **4 MB of memory.** An uncompressed A4 page at 600 dpi is ~4.3 MB of
   raster. Modern drivers do not know or care.
4. **Windows cannot pick a driver by itself.** In-box class-driver ranking
   relies on the `CID:` key in the IEEE-1284 device ID, a Microsoft
   extension from after 2012. A printer from 2000 obviously has none.

**Conclusion:** feed the printer conservative **PCL5e raster** — the dialect
it was sold with.

---

## What's in the repository

```
driver/                 the printer driver
  ml1250.inf            INF package
  ml1250.gpd            printer description for Unidrv
  Install-ML1250.ps1    signs the package and installs it
  Uninstall-ML1250.ps1  removes everything, including the certificate
  Install.cmd           "right-click → Run as administrator"
  Uninstall.cmd
src/                    source of the ml1250.exe tool
tests/                  self-tests, INF and GPD checkers
research/               small standalone experiments with usbprint.sys
                        and RAW spooling that the tool grew out of
build.cmd, Makefile     build ml1250.exe into dist/
```

### The driver: INF + GPD, not a single line of binary code

This is a **v3 Unidrv minidriver**. All the work — rasterising,
halftoning, banding, raster compression — is done by Windows' own in-box
`unidrv.dll`; `ml1250.gpd` only describes which bytes the printer
understands. Nothing third-party is loaded into the spooler, and the same
package works on x64 and Arm64 because there is nothing to compile.

The INF structure is copied from Microsoft's `prnge001.inf` — the INF of
the in-box "Generic / Text Only" driver, itself a GPD-only Unidrv
minidriver. Hence the modern `CoreDriverSections` form instead of the old
`Include=ntprint.inf` / `Needs=`.

Key decisions in the GPD, and why:

* **PCL5e, not PCL6.** Smaller, older and more predictable.
* **No device fonts** — Unidrv rasterises all text. Slower, but Cyrillic
  and any other font print exactly as on screen, with no symbol-set or
  font-substitution surprises.
* **Both `CmdEnableTIFF4` and `CmdEnableDRC` are declared**, so Unidrv
  compresses every row both ways and keeps the shorter one.
* **`BadCursorMoveInGrxMode` is declared for all four directions.** While
  raster is in progress, PCL accepts nothing but data transfer, compression
  changes and Y offset; any other command implicitly ends the image.
  Without this line Unidrv would put `<Esc>*p` between bands and silently
  break the picture; with it, Unidrv properly ends the raster, moves the
  cursor and starts a new one. For the same reason `StripBlanks` is limited
  to `LEADING, TRAILING` — there is nothing to jump over a gap in the
  middle of a raster row with.
* **Default halftone is `HT_PATSIZE_6x6_M`** (clustered dither). Not for
  looks: clustered patterns produce long runs of equal bytes, while HP
  explicitly names error diffusion as the case a PCL5 printer's internal
  compression cannot handle. Measured on an A4 600 dpi page with a
  gradient (uncompressed raster: 3,979,376 bytes):

  | halftone | job size |
  |---|---|
  | clustered dither | **182,522** |
  | error diffusion | **3,705,322** |

  A twentyfold difference. On a 4 MB printer that is exactly the line
  between a printed page and a `print overrun`.
* **300 dpi is a real option, not a fallback.** The driver also sends
  `@PJL SET RESOLUTION=300`, so the printer's page buffer shrinks
  fourfold. This is the first thing to switch if a complex page fails.
* **Geometry comes from the PCL 5 logical page table.** For A4 at 600 dpi
  the printable area is 4676 × 6814 dots at offset (142, 100). The PCL
  logical page is narrower than the sheet, a common source of margins off
  by a quarter inch.

### Unidrv pitfalls that crashed the driver

The GPD parser reports no errors: a wrong line is either silently dropped
or crashes the spooler later, at print time.

* **A bare `%` in a command crashes the spooler.** In the compiled GPD a
  parameter reference is stored as `%0001`, and inside GPD quotes `%` is an
  escape (`%%` is a percent sign, `%<` a literal `<`). The UEL was written
  in hex as `<1B252D3132...>`, so the `25` byte went into the binary
  unescaped. Unidrv read `%-123` as parameter number −123 and crashed in
  `IProcessTokenStream` (`spoolsv.exe`, `UNIDRV.DLL+0x64025`, access
  violation) on the very first command of every job. The correct form is
  `"<1B>%%-12345X"`, as in Microsoft's own `PJL.GPD`.
* **Two entries on one line are silently dropped.** An entry's value runs
  to the end of the line, so in
  `*Command: CmdSelect { *Order: DOC_SETUP.10 *Cmd: "<1B>&l26A" }`
  the `*Cmd` is swallowed by `*Order` and the command disappears. Every
  paper size, tray and PJL option was lost this way. A single entry in
  one-line braces (`*Command: CmdCR { *Cmd : "<0D>" }`) is fine.
* **The page size command resets the PCL top margin** to 1/2 inch, so
  `<Esc>&l0E` is sent in `CmdStartPage`, after the page size, not before.

`tests/gpdcheck.py` now catches the first two.

---

## Installation (USB)

1. **Connect the printer and switch it on first.** Its `USB00x` port is
   published by the in-box `usbprint.sys` only while Windows sees the
   device, regardless of any driver.
2. Copy the `driver` folder to the target machine.
3. Run the check first; it changes nothing:
   ```powershell
   .\Install-ML1250.ps1 -CheckOnly
   ```
4. Right-click `Install.cmd` → **Run as administrator**.

Or by hand:
```powershell
.\Install-ML1250.ps1 -PrinterName "Samsung ML-1250"
```

The installer finds the ML-1250's port by its USB VID/PID, so another USB
printer on the same machine does not confuse it. Pass `-PortName` to
override, e.g. `-PortName LPT1:` for a parallel cable.

### About signing — read this

Windows **will not install** an unsigned printer driver package. The
spooler's `ConfigureDriverValidationLevel` policy has no "allow unsigned"
level: the most lenient (4, the default) is "Allow all validly **signed**
drivers". `pnputil` has no override, and `bcdedit /set testsigning on`
does not help — that switch is for kernel-mode code, and this package
contains no code at all.

So the installer **creates a self-signed certificate and adds it to this
machine's Trusted Root and Trusted Publishers stores.** That is a real
trust decision: anything signed with that key becomes trusted system-wide.
Mitigations:

* a 3072-bit key that exists only while signing and is deleted right
  after (unless you pass `-KeepCertificate`);
* `Uninstall-ML1250.ps1` removes the certificate from all stores;
* exactly two text files are signed, `ml1250.inf` and `ml1250.gpd`.

If that is unacceptable, use the `ml1250.exe` tool (below); it prints with
no driver at all.

### Windows protected print mode

The one thing that makes the driver impossible to install. When it is on,
Windows removes third-party print drivers from the driver store and allows
only IPP. It is **off** by default on 24H2/25H2, but Microsoft has said it
will become the default in the future. The installer checks for it and
refuses to run rather than half-install.

To turn it off: `Settings → Bluetooth & devices → Printers & scanners →
Printer preferences → Windows protected print mode → Off`. If it is
enforced by group policy, only a domain administrator can change it.

---

## If it does not print

A ladder, top to bottom. Each step rules out half of the possibilities.

**0. Do not bother with the in-box "Microsoft PCL6 Class Driver".**
It ships with Windows 11 (`prnms005.inf`) and looks tempting, but its
renderer `MSxpsPCL6.dll` emits the header `) HP-PCL XL;3;0;` — protocol
class 3.0, while the ML-1250 implements only 1.1 and 2.0. It also binds by
the `1284_CID_MS_PCL6` ID, which a printer from 2000 does not have. A dead
end by construction.

**1. Are the printer and the link alive at all?** Checked with no driver
and none of our raster — the printer prints its own page:
```
ml1250 list                      # is the printer visible as a USB device?
ml1250 probe --pjl               # what does it report about itself?
ml1250 testprint config --usb    # the printer's configuration page
```
`testprint` sends only `@PJL TEST PRINT = CONFIG` — not one byte of our
content. If the page comes out, the port, cable, PJL and mechanics are
fine, and anything that breaks later is ours. As a bonus, the page shows
the real amount of memory and the printer's list of personalities.

**2. Is the PCL interpreter alive?**
```
ml1250 testpage --text --usb     # text in the printer's own fonts
```

**3. Our raster.**
```
ml1250 testpage --usb            # frame, grey scale, lines of various widths
```
The frame shows the real printable area. If the image is shifted or
clipped, adjust margins with `--offset-x` / `--offset-y` (in dots, 600 per
inch), and in the driver with the same numbers in `*PrintableOrigin` /
`*CursorOrigin`.

**4. A complex page does not fit in memory** — symptom: the page does not
come out, comes out half-printed, or the printer blinks an error.
In printing preferences: **300 dpi**, halftone **6x6**, not error
diffusion. Or add memory — there is a SIMM slot underneath, up to 68 MB;
after that it makes sense to turn on "Page protection".

**5. The queue hangs / odd behaviour through a USB→LPT adapter.**
Use the **"Samsung ML-1250 PCL5e"** model (the default) instead of
"(with status)" — the latter adds a PJL language monitor, which needs a
genuinely bidirectional port.

**6. Nothing helped — print around the driver.**
```
ml1250 print page.pgm --usb
```
Any PDF/PS can be rasterised with Ghostscript and sent directly:
```
gswin64c -q -dBATCH -dNOPAUSE -sDEVICE=pgmraw -r600 -sOutputFile=- file.pdf | ml1250 print - --usb
```

---

## The `ml1250.exe` tool

A standalone exe with no dependencies. It talks to the printer directly
through `usbprint.sys` (`GUID_DEVINTERFACE_USBPRINT`), through `\\.\LPT1`,
or through an existing Windows queue in RAW mode.

```
ml1250 list                          USB printers and their IEEE-1284 IDs
ml1250 probe [--pjl]                 what the printer says about itself + status
ml1250 send FILE [target]            send a file byte for byte
ml1250 print FILE... [opts] [target] render and print
ml1250 testpage [opts] [target]      our diagnostic page
ml1250 testprint config|demo|pclfont the printer's own page (pure PJL)
```

Where to print: `-o FILE` | `--usb [N]` | `--dev PATH` | `--lpt 1` | `--queue "Name"`

Job options: `--lang pcl5|pclxl|smartgdi`, `--paper`, `--res 600|300`,
`--copies`, `--density`, `--economode`, `--compress auto|0|2|3`,
`--halftone cluster|bayer|fs|threshold`, `--offset-x`, `--offset-y`,
`--ret normal|enhance|gray|gray+` (this is "SRT"),
`--pageprotect auto|on|off` (not sent at all by default),
`--xl-class 2.0|1.1`, `--no-pjl`.

Input formats: Netpbm (`.pbm` / `.pgm` / `.ppm`) and **PWG Raster** — the
format Windows sends to IPP printers, so the tool also works as a
converter behind an IPP bridge.

`probe` is the most useful command: it reads the IEEE-1284 device ID via
`IOCTL_USBPRINT_GET_1284_ID`, and the `CMD:` field settles which languages
the printer really understands.

### `--lang smartgdi`

Output in Samsung SmartGDI (SPL1) — **not for the ML-1250**, but for its
same-chassis siblings ML-1210 / ML-1220M and for the ML-4500 / ML-5000A /
ML-6040 family, which really are host-based. The format is reconstructed
from Ghostscript's `contrib/gdevgdi.c`, the only public description. It is
also kept as insurance in case a particular unit turns out to have a GDI
board.

---

## Building

Any Windows C compiler that can link `setupapi` and `winspool`. Tested with
llvm-mingw clang 22.

```
build.cmd                 # clang / gcc / MSVC, whichever is found
make && make test         # if you have make
```

The driver itself (`driver/`) needs no building — it is text files.

---

## What has been verified

The driver has been tested on a real ML-1250 under Windows 10 22H2
(build 19045). Everything else, honestly, item by item.

**Verified:**

* Printing through the driver. Installation, a queue on the ML-1250's USB
  port next to another USB printer, and a GDI test page — A4, 600 dpi,
  frame on the edge of the printable area, Cyrillic text, grey scale,
  lines — came out of the printer correctly. The driver's output at
  600 dpi, 300 dpi and in landscape was captured with "print to file",
  decoded and rendered back: the command order matches `ml1250.exe`, and
  there are no forbidden commands inside raster graphics.
* Byte streams. Every compressor has an independent decoder, and a full
  PCL5e job is decoded back into a bitmap and compared with the source
  pixel by pixel, in every compression mode (0 / 2 / 3 / auto).
  `make test` → 42 checks.
* The PCL-XL stream was taken apart byte by byte; every operator,
  attribute and data-type tag was checked against Microsoft's `PCLXL.GPD`
  in `C:\Windows\System32\spool\drivers\x64\3\`.
* `ml1250.inf` is parsed by Windows' own parser (SetupAPI,
  `INF_STYLE_WIN4`) — `tests/infcheck.c`; section names are checked for
  collisions with `ntprint.inf`. The INF structure is copied from a live
  `prnge001.inf`, and the core section names and GUID from `ntprint.inf`
  of the same Windows build.
* `ml1250.gpd` passes the structural check (`tests/gpdcheck.py`):
  balanced braces, every `*DefaultOption` names an existing option, every
  command name is in the Unidrv vocabulary, `*Order` sections are valid,
  hex blocks are whole, no two entries share a line and no bare `%` in
  commands (see "Unidrv pitfalls" above — the parser swallows both
  silently).
* Everything needed is present in Windows 11 25H2 (build 26200):
  `UNIDRV.DLL`, `UNIDRVUI.DLL`, `UNIRES.DLL`, `STDNAMES.GPD`,
  `PJLMON.DLL`, and the `[UNIDRV.OEM]` / `[UNIDRV_DATA]` sections in
  `ntprint.inf`.
* The installer's read-only mode (`-CheckOnly`).

**Not verified:**

* Margins on paper sizes other than A4. The geometry comes from the PCL 5
  logical page table; a constant offset, if any, is fixed with
  `--offset-x/--offset-y` in the tool and `*PrintableOrigin` /
  `*CursorOrigin` in the GPD.
* How PJL variables behave on this hardware. `@PJL SET ECONOMODE` and
  `@PJL SET RET` are taken from strings in Samsung's own driver, so they
  are almost certainly right; `PAGEPROTECT` is not sent by default
  precisely because it is unverified. By the PJL spec, unknown variables
  are ignored with a warning, and there is `--no-pjl`.
* Printer replies to PJL queries (`probe --pjl`). Samsung used the
  non-standard `@PJL INFO PRINTER = ALL`; the tool tries it and the HP
  standard ones, but silence from the printer is a normal outcome.
* The "(with status)" variant with the PJL language monitor.
* Installation on Windows 11.
* The `--lang pclxl` path.

---

## Sources

* Samsung, *Service Manual, Laser Printer ML-1200 Series
  (ML-1210 / ML-1250 / ML-1220M)* — spec table, block diagrams, board list.
* Samsung, *ML-1250 User's Manual* — emulation, fonts, memory, the PCL 5e
  menu, Linux instructions.
* Samsung's original ML-1250/250 driver CD (JC46-00127A, 2002):
  `ML125.INI`, `ML125.DDF`, `USB/SSUSB.INF`, `LINUX/SSPRT.DB`,
  `LINUX/README.TXT` — PCL-XL protocol classes, USB VID/PID and the
  vendor's own mapping of the model to `ljet4`/`pxlmono`.
* HP, *PCL 5 Printer Language Technical Reference Manual* and
  *PCL 5 Comparison Guide* — raster commands, compression, the logical
  page table, behaviour when memory runs out.
* HP, *PCL XL Feature Reference, Protocol Class 2.0*; tags checked against
  `PCLXL.GPD` shipped with Windows.
* Microsoft, Unidrv/GPD documentation (`windows-driver-docs`), with a live
  `prnge001.inf` + `ntprint.inf` from Windows 11 25H2 as the working model.
* Ghostscript, `contrib/gdevgdi.c` — the Samsung SmartGDI format.
* OpenPrinting foomatic-db — classification of the ML-1250 as a PCL
  printer.

---

## License

[0BSD](LICENSE) — do whatever you want with it: use, copy, modify, port,
redistribute, sell, with or without attribution. No warranty.

Samsung, HP, Microsoft and Windows are trademarks of their respective
owners. This project is not affiliated with or endorsed by any of them.
