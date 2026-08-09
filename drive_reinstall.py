#!/usr/bin/env python3
"""Fresh NT10 NTFS install -> HD boot -> first-boot wizard -> installed desktop.
Captures screenshots and scans the serial log for NTFS / rendering trouble.
Run from the reactos repo root with DISPLAY set for the GTK window."""
import sys, time, os, re
from ros_test import (RosQemu, install_ntfs, monitor_copy, finish_install,
                      boot_from_hd, drive_first_boot_wizard)

NAME = "nt10reinstall"
ISO = "build_nt10/bootcd.iso"

def scan_ntfs(q, tag):
    lines = q.log_lines()
    pats = ["ntfs", "Ntfs", "NTFS", "STATUS_FILE_CORRUPT", "FILE_SYSTEM",
            "MFT", "0x24", "0x26", "0x23"]
    hits = [l.strip() for l in lines
            if any(p in l for p in pats) and "err" in l.lower()
            or "Ntfs" in l and ("ASSERT" in l or "fail" in l.lower())]
    print(f"--- [{tag}] NTFS/FS error-ish lines: {len(hits)} ---")
    for h in hits[-40:]:
        print(f"   {h}")
    # any crash markers
    c = q.crashed()
    print(f"--- [{tag}] crashed()={c} ---")
    return hits

def main():
    print("=== STEP 1: fresh disk + boot installer ===", flush=True)
    q = RosQemu(NAME, ISO)            # fresh 10G raw disk
    q.start(boot="d")

    install_ntfs(q)
    ok = monitor_copy(q)
    print(f"monitor_copy -> {ok}", flush=True)
    if not ok:
        print("FILE_COPY failed/stalled; dumping tail", flush=True)
        for l in q.log_lines()[-30:]:
            print("   ", l.rstrip())
        return

    finish_install(q)
    print("=== install reboot reached; install log NTFS scan ===", flush=True)
    scan_ntfs(q, "post-install")

    print("=== STEP 2: boot from HD (first boot) ===", flush=True)
    booted = boot_from_hd(q)
    print(f"boot_from_hd(first) -> {booted}", flush=True)
    q.screenshot("firstboot")
    if not booted:
        scan_ntfs(q, "firstboot-fail")
        return

    print("=== STEP 3: drive first-boot wizard ===", flush=True)
    wiz = drive_first_boot_wizard(q)
    print(f"wizard -> {wiz}", flush=True)
    scan_ntfs(q, "post-wizard")

    print("=== STEP 4: boot installed system (second boot) ===", flush=True)
    # wizard finalize ends in a guest reboot -> qemu exited; relaunch from HD.
    if q.is_alive():
        print("  guest still alive after wizard; reusing", flush=True)
    else:
        booted2 = boot_from_hd(q)
        print(f"boot_from_hd(second) -> {booted2}", flush=True)

    # Let the desktop settle, then capture rendering.
    time.sleep(25)
    for i in range(3):
        p, _ = q.screenshot(f"desktop{i}")
        print(f"  screenshot -> {p}", flush=True)
        time.sleep(8)
    scan_ntfs(q, "desktop")
    print("=== DONE; qemu left running for inspection ===", flush=True)

if __name__ == "__main__":
    main()
