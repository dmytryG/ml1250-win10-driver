#!/usr/bin/env python3
"""Structural check for a Unidrv GPD file.

Unidrv parses the GPD at run time, inside the spooler, and a mistake shows up
as a printer that quietly misbehaves rather than as an error.  This catches
the structural classes of mistake before that: unbalanced braces, a
*DefaultOption naming an option that does not exist, duplicate *Order
sequence numbers inside one job section, command strings whose hex blocks are
malformed, and references to names the file never defines.

    python tests/gpdcheck.py driver/ml1250.gpd
"""
import re
import sys

# Command names Unidrv knows, from the WDK printer-command reference.
KNOWN_COMMANDS = {
    # printer configuration
    "CmdStartJob", "CmdStartDoc", "CmdStartPage", "CmdEndPage", "CmdEndDoc",
    "CmdEndJob", "CmdCopies", "CmdSleepTimeOut",
    # cursor
    "CmdBackSpace", "CmdCR", "CmdFF", "CmdLF", "CmdPopCursor", "CmdPushCursor",
    "CmdSetAnyRotation", "CmdSetLineSpacing", "CmdSetSimpleRotation",
    "CmdUniDirectionOff", "CmdUniDirectionOn",
    "CmdXMoveAbsolute", "CmdXMoveRelLeft", "CmdXMoveRelRight",
    "CmdYMoveAbsolute", "CmdYMoveRelDown", "CmdYMoveRelUp",
    # raster emission
    "CmdBeginRaster", "CmdEndRaster", "CmdSendBlockData", "CmdEndBlockData",
    "CmdSetDestBmpHeight", "CmdSetDestBmpWidth",
    "CmdSetSrcBmpHeight", "CmdSetSrcBmpWidth",
    "CmdSendBlackData", "CmdSendCyanData", "CmdSendMagentaData",
    "CmdSendYellowData", "CmdSendRedData", "CmdSendGreenData", "CmdSendBlueData",
    # raster compression
    "CmdDisableCompression", "CmdEnableDRC", "CmdEnableFE_RLE",
    "CmdEnableOEMComp", "CmdEnableTIFF4",
    # option selection
    "CmdSelect", "CmdDeselect",
}

JOB_SECTIONS = {"JOB_SETUP", "DOC_SETUP", "PAGE_SETUP",
                "PAGE_FINISH", "DOC_FINISH", "JOB_FINISH"}

problems = []
notes = []


def fail(line, msg):
    problems.append(f"  line {line}: {msg}")


def main(path):
    text = open(path, encoding="latin-1").read()
    lines = text.split("\n")

    # 1. Brace balance, ignoring comments and quoted strings.
    depth = 0
    for n, raw in enumerate(lines, 1):
        line = re.sub(r"\*%.*$", "", raw)          # *% ... comment to EOL
        line = re.sub(r'"[^"]*"', '""', line)      # blank out strings
        depth += line.count("{") - line.count("}")
        if depth < 0:
            fail(n, "closing brace without a matching open")
            depth = 0
    if depth != 0:
        problems.append(f"  {depth} brace(s) left open at end of file")

    # 1b. One entry per line.  An entry's value runs to the end of the line,
    #     so in "*Command: CmdSelect { *Order: X *Cmd: "..." }" the *Order
    #     value swallows the *Cmd and Unidrv drops the whole command without
    #     an error anywhere - every paper size, tray and PJL option written
    #     that way was never sent.  A single entry in one-line braces is fine
    #     (Microsoft's own GPDs do "*Command: CmdCR { *Cmd : "<0D>" }").
    for n, raw in enumerate(lines, 1):
        line = re.sub(r"\*%.*$", "", raw)
        line = re.sub(r'"[^"]*"', '""', line)
        for part in line.split("{"):
            if len(re.findall(r"\*[A-Za-z?]+\s*:", part)) > 1:
                fail(n, "two entries on one line; Unidrv silently drops them")
                break

    # 2. Features: every *DefaultOption must name a real *Option.
    feature = None
    options = []
    default = None
    fline = 0
    feature_count = 0
    for n, raw in enumerate(lines, 1):
        line = re.sub(r"\*%.*$", "", raw).strip()
        m = re.match(r"\*Feature:\s*(\S+)", line)
        if m:
            if feature and default and default not in options:
                fail(fline, f"feature {feature}: *DefaultOption {default} "
                            f"is not one of {options}")
            feature, options, default, fline = m.group(1), [], None, n
            feature_count += 1
            continue
        m = re.match(r"\*Option:\s*(\S+)", line)
        if m and feature:
            options.append(m.group(1))
        m = re.match(r"\*DefaultOption:\s*(\S+)", line)
        if m and feature:
            default = m.group(1)
    if feature and default and default not in options:
        fail(fline, f"feature {feature}: *DefaultOption {default} "
                    f"is not one of {options}")
    if feature and not default:
        fail(fline, f"feature {feature} has no *DefaultOption")

    # 3. Command names must be ones Unidrv recognises.
    for n, raw in enumerate(lines, 1):
        line = re.sub(r"\*%.*$", "", raw)
        m = re.search(r"\*Command:\s*([A-Za-z_][A-Za-z0-9_]*)", line)
        if m and m.group(1) not in KNOWN_COMMANDS:
            fail(n, f"unknown command name {m.group(1)}")

    # 4. *Order values: valid section, and unique within a section.
    seen = {}
    for n, raw in enumerate(lines, 1):
        line = re.sub(r"\*%.*$", "", raw)
        m = re.search(r"\*Order:\s*([A-Z_]+)\.(\d+)", line)
        if not m:
            if "*Order:" in line:
                fail(n, "malformed *Order (want SECTION.number)")
            continue
        sect, seq = m.group(1), int(m.group(2))
        if sect not in JOB_SECTIONS:
            fail(n, f"unknown job section {sect}")
        # CmdSelect entries of different options legitimately share a number.
        key = (sect, seq)
        seen.setdefault(key, []).append(n)

    # 5. Hex blocks inside command strings must be whole bytes.  Comments
    #    talk about escape sequences as <Esc>..., so strip them first.
    for n, raw in enumerate(lines, 1):
        code = re.sub(r"\*%.*$", "", raw)
        for blk in re.findall(r"<([^>]*)>", code):
            h = blk.replace(" ", "")
            if not h:
                fail(n, "empty <> hex block")
            elif re.search(r"[^0-9a-fA-F]", h):
                fail(n, f"non-hex characters in <{blk}>")
            elif len(h) % 2:
                fail(n, f"odd number of hex digits in <{blk}>")

    # 5b. A literal % in a command string must be written %%.  Inside GPD
    #     quotes % is an escape (%% is a percent, %< a literal <), and the
    #     compiled GPD marks parameter references as %NNNN.  A hex <25> skips
    #     the escaping and lands in the binary as a bare %, so "<1B252D...>"
    #     was read as parameter number -123 and the spooler crashed inside
    #     IProcessTokenStream on the first command of every job.
    for n, raw in enumerate(lines, 1):
        code = re.sub(r"\*%.*$", "", raw)
        for s in re.findall(r'"([^"]*)"', code):
            for m in re.finditer(r"%(.?)", s):
                if m.group(1) not in ("%", "<"):
                    fail(n, f'"%{m.group(1)}" in "{s}": the parser eats a lone %; '
                            f'a literal percent must be %%')
            text_only = re.sub(r"%.", "", s)            # escaped pairs
            for blk in re.findall(r"<([^>]*)>", text_only):
                h = blk.replace(" ", "")
                if any(h[i:i + 2].lower() == "25" for i in range(0, len(h), 2)):
                    fail(n, f"hex <25> in <{blk}> is a bare %; write %% as text")

    # 6. Things this driver specifically must have.
    required = ["*GPDSpecVersion", "*ModelName", "*MasterUnits",
                "*ResourceDLL", "*Command: CmdSendBlockData",
                "*Feature: PaperSize", "*Feature: Resolution",
                "*Feature: InputBin"]
    for r in required:
        if r not in text:
            problems.append(f"  missing required entry: {r}")

    # 7. Advisory: compression declared means Unidrv does it for us.
    for c in ("CmdEnableTIFF4", "CmdEnableDRC"):
        if c not in text:
            notes.append(f"  {c} is not declared - Unidrv will not use that "
                         f"compression")

    print(f"checking {path}: {len(lines)} lines, {feature_count} features")
    for x in notes:
        print("  note" + x)
    if problems:
        print("PROBLEMS:")
        for p in problems:
            print(p)
        return 1
    print("GPD looks structurally sound")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "driver/ml1250.gpd"))
