#!/usr/bin/env python3
"""Diagnostic: reinstall + boot, then report which MM path touches font pages.
Greps the serial log for:
  SYSMAP++      -> MmMapViewOfSegment kernel-space view increment (did font view hit it?)
  FONTPAGEOUT   -> MmpPageOutPhysicalAddress saw a font-file segment (is trimmer the path?)
  Sparing       -> our guard fired
  glyphSize.cx of zero -> font bug surface
"""
import sys, time, subprocess
from ros_test import RosQemu, finish_install, boot_from_hd

NAME = "nt10diag"; ISO = "build_nt10/bootcd.iso"

def shot(q, n):
    p,_ = q.screenshot(n)
    subprocess.run(["convert", p, p.replace(".ppm",".png")], capture_output=True)
    return p

def main():
    q = RosQemu(NAME, ISO)
    q.start(boot="d")
    print("=== boot menu -> debug setup ===", flush=True)
    q.send("home"); time.sleep(0.3)
    for _ in range(3): q.send("down"); time.sleep(0.15)
    q.send("ret")
    q.wait("SourcePath (1):", timeout=120); time.sleep(2)
    print("=== intro -> partition ===", flush=True)
    for _ in range(3): q.send("ret"); time.sleep(0.9)
    q.send("ret"); time.sleep(1.5); shot(q, "part")
    print("=== install -> format list ===", flush=True)
    q.send("ret"); time.sleep(2.5); shot(q, "fmtlist")
    print("=== NTFS quick (2 down) -> format ===", flush=True)
    q.send("down"); time.sleep(0.4); q.send("down"); time.sleep(0.4); shot(q, "fmtsel")
    q.send("ret"); time.sleep(1.2); q.send("ret")
    q.wait("FormatState: FormatDone", timeout=180); print("  FormatDone", flush=True)
    print("=== file copy (silent) ===", flush=True)
    time.sleep(100); shot(q, "bootldr")
    print("=== finish_install ===", flush=True)
    try: finish_install(q)
    except Exception as e: print(f"finish_install raised: {e!r}", flush=True)
    print("=== first HD boot ===", flush=True)
    booted = boot_from_hd(q); print(f"boot_from_hd -> {booted}", flush=True)
    time.sleep(45); shot(q, "hddesk")

    lines = q.log_lines()
    def grab(tag): return [l.strip() for l in lines if tag in l]
    watch = grab("WATCH-FREE") + grab("WATCH-ZERO") + grab("FTWATCH") + grab("WATCH set")
    sysmap = grab("SYSMAP++"); fpo = grab("FONTPAGEOUT"); ff = grab("FONTFAULT")
    ftmap = grab("FTMAP")
    print("--- WATCH (producer stack capture) ---", flush=True)
    # dump the watch lines plus following stack frames from raw log
    inwatch = False
    for l in lines:
        s = l.rstrip()
        if any(t in s for t in ("FTWATCH","WATCH set","WATCH-FREE","WATCH-ZERO")):
            print("   ", s.strip(), flush=True); inwatch = ("WATCH-FREE" in s or "WATCH-ZERO" in s)
        elif inwatch and ("<" in s or "0x" in s.lower() or ".c:" in s or "+0x" in s):
            print("   ", s.strip(), flush=True)
        else:
            inwatch = False
    spare  = grab("Sparing system-mapped"); zg = sum(1 for l in lines if "glyphSize.cx of zero" in l)
    print(f"=== COUNTS: FTMAP={len(ftmap)} SYSMAP++={len(sysmap)} FONTFAULT={len(ff)} FONTPAGEOUT={len(fpo)} sparing={len(spare)} zero-glyph={zg} ===", flush=True)
    print("--- FTMAP (map-time + after-parse buffer samples) ---", flush=True)
    for l in ftmap[:60]: print("   ", l, flush=True)
    print("--- FONTFAULT (off/raw/len/vdl) ---", flush=True)
    for l in ff[:30]: print("   ", l, flush=True)
    print("--- SYSMAP++ (font-relevant) ---", flush=True)
    for l in sysmap:
        if any(x in l.lower() for x in ("ttf","ttc","otf","font",".fon")): print("   ", l, flush=True)
    print("--- FONTPAGEOUT (all) ---", flush=True)
    for l in fpo[:40]: print("   ", l, flush=True)
    print("=== DONE ===", flush=True)

if __name__ == "__main__":
    main()
