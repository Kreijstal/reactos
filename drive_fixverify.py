#!/usr/bin/env python3
"""Verify the system-mapped-segment purge fix: fresh NTFS reinstall + HD boot,
then report the count of 'glyphSize.cx of zero' Tahoma warnings (must be 0 if
fixed) and capture a desktop screenshot for legibility confirmation."""
import time, subprocess
from ros_test import RosQemu, finish_install, boot_from_hd

NAME = "nt10diag"; ISO = "build_nt10/bootcd.iso"

def shot(q, n):
    p,_ = q.screenshot(n)
    subprocess.run(["convert", p, p.replace(".ppm",".png")], capture_output=True)
    return p.replace(".ppm",".png")

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
    time.sleep(50); png = shot(q, "hddesk")

    lines = q.log_lines()
    zg = sum(1 for l in lines if "glyphSize.cx of zero" in l)
    tah = sum(1 for l in lines if "glyphSize.cx of zero" in l and "Tahoma" in l)
    print(f"=== RESULT: zero-glyph-warnings={zg} (Tahoma={tah}) desktop_png={png} ===", flush=True)
    print("=== DONE ===", flush=True)

if __name__ == "__main__":
    main()
