#!/usr/bin/env python3
"""Verify the system-mapped-segment pageout fix end-to-end.

Reinstalls NT10 onto fresh NTFS with the fixed kernel/bootcd, boots from HD, and
counts at the logon render:
  - 'glyphSize.cx of zero'           -> font bug surface (expect ~0 if fixed)
  - 'Sparing system-mapped segment'  -> the producer firing + being spared
If glyph-zero drops to ~0 AND sparing fires, the producer is confirmed and fixed.
"""
import sys, time, subprocess
from ros_test import RosQemu, finish_install, boot_from_hd

NAME = "nt10spare"; ISO = "build_nt10/bootcd.iso"

def shot(q, n):
    p,_ = q.screenshot(n)
    subprocess.run(["convert", p, p.replace(".ppm",".png")], capture_output=True)
    return p

def main():
    q = RosQemu(NAME, ISO)          # fresh 10G raw disk
    q.start(boot="d")

    print("=== boot menu -> debug setup ===", flush=True)
    q.send("home"); time.sleep(0.3)
    for _ in range(3): q.send("down"); time.sleep(0.15)
    q.send("ret")
    q.wait("SourcePath (1):", timeout=120); time.sleep(2)

    print("=== intro pages -> partition screen ===", flush=True)
    for _ in range(3): q.send("ret"); time.sleep(0.9)
    q.send("ret"); time.sleep(1.5)
    shot(q, "part")

    print("=== install on unpartitioned -> format list ===", flush=True)
    q.send("ret"); time.sleep(2.5)
    shot(q, "fmtlist")

    print("=== select NTFS quick (2 down) -> format ===", flush=True)
    q.send("down"); time.sleep(0.4); q.send("down"); time.sleep(0.4)
    shot(q, "fmtsel")
    q.send("ret"); time.sleep(1.2)
    q.send("ret")
    q.wait("FormatState: FormatDone", timeout=180)
    print("  FormatDone reached", flush=True)

    print("=== let file copy finish (silent) ===", flush=True)
    time.sleep(100)
    shot(q, "bootldr")

    print("=== finish_install ===", flush=True)
    try:
        finish_install(q)
    except Exception as e:
        print(f"finish_install raised: {e!r}", flush=True)

    print("=== first HD boot (font reads at win32k init) ===", flush=True)
    booted = boot_from_hd(q)
    print(f"boot_from_hd -> {booted}", flush=True)
    print("  waiting for win32k font load + logon render", flush=True)
    time.sleep(45)
    shot(q, "hddesk")

    lines = q.log_lines()
    zglyph = sum(1 for l in lines if "glyphSize.cx of zero" in l)
    spare  = [l.strip() for l in lines if "Sparing system-mapped segment" in l]
    print(f"=== RESULT: zero-glyph warnings={zglyph} ; sparing-events={len(spare)} ===", flush=True)
    for l in spare[:30]:
        print("   ", l, flush=True)
    if zglyph == 0 and spare:
        print("=== VERDICT: FIXED + producer confirmed (sparing fired, text renders) ===", flush=True)
    elif zglyph == 0:
        print("=== VERDICT: text renders, but no sparing logged - investigate ===", flush=True)
    else:
        print(f"=== VERDICT: still broken ({zglyph} zero-glyph) - not the (only) producer ===", flush=True)
    print("=== DONE ===", flush=True)

if __name__ == "__main__":
    main()
