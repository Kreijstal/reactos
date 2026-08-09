#!/usr/bin/env python3
"""Reinstall with the GLYPHDBG-instrumented ntfs.sys, boot from HD, and capture
the NtfsReadFile paging-read trace for *.ttf so we can see whether glyph render
pages come back zero. Drives text-setup with the exact sequence proven manually
(this build emits no PROGRESS:/early SETUP_SELECTED: markers)."""
import sys, time, os, subprocess
from ros_test import RosQemu, finish_install, boot_from_hd

NAME = "nt10glyphdbg"; ISO = "build_nt10/bootcd.iso"

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
    q.send("ret"); time.sleep(1.5)          # land on partition list
    shot(q, "part")

    print("=== install on unpartitioned -> format list ===", flush=True)
    q.send("ret"); time.sleep(2.5)
    shot(q, "fmtlist")

    print("=== select NTFS quick (2 down) -> format ===", flush=True)
    q.send("down"); time.sleep(0.4); q.send("down"); time.sleep(0.4)
    shot(q, "fmtsel")
    q.send("ret"); time.sleep(1.2)          # -> "press ENTER to format"
    q.send("ret")                            # confirm format
    q.wait("FormatState: FormatDone", timeout=180)
    print("  FormatDone reached", flush=True)

    print("=== let file copy finish (silent) ===", flush=True)
    time.sleep(100)
    shot(q, "bootldr")

    print("=== finish_install (mashes ENTER thru bootloader MBR+VBR + success) ===", flush=True)
    try:
        finish_install(q)
    except Exception as e:
        print(f"finish_install raised: {e!r}", flush=True)

    print("=== first HD boot (font reads happen at win32k init) ===", flush=True)
    booted = boot_from_hd(q)
    print(f"boot_from_hd -> {booted}", flush=True)
    print("  waiting for win32k font load + logon render", flush=True)
    time.sleep(45)
    shot(q, "hddesk")

    # Capture GLYPHDBG + zero-glyph evidence
    lines = q.log_lines()
    fontdbg = [l.strip() for l in lines if "GLYPHDBG" in l]
    zglyph  = sum(1 for l in lines if "glyphSize.cx of zero" in l)
    print(f"=== GLYPHDBG lines: {len(fontdbg)} ; zero-glyph warnings: {zglyph} ===", flush=True)
    for l in fontdbg[:120]:
        print("   ", l, flush=True)
    print("=== DONE ===", flush=True)

if __name__ == "__main__":
    main()
