#!/usr/bin/env python3
"""From the bootloader-install screen: install MBR+VBR, finish, reboot,
boot from HD, drive first-boot wizard, second boot, capture installed desktop.
This build emits no PROGRESS: markers, so we anchor on the markers it DOES emit
(SETUP_PAGE:SUCCESS / Install NTFS bootcode / NtfsShutdown) and on screenshots."""
import sys, time, os
from ros_test import (RosQemu, finish_install, boot_from_hd,
                      drive_first_boot_wizard)

NAME = "nt10reinstall"; ISO = "build_nt10/bootcd.iso"
PID = int(sys.argv[1])

def scan_fs(q, tag):
    hits = []
    for l in q.log_lines():
        ll = l.lower()
        if ("ntfs" in ll or "fsctl" in ll or "fastfat" in ll) and \
           ("err" in ll or "fail" in ll or "assert" in ll or "corrupt" in ll):
            hits.append(l.strip())
        if any(m in l for m in ("*** STOP", "BugCheck", "kdb:>", "Assertion")):
            hits.append(l.strip())
    print(f"--- [{tag}] FS-error/crash lines: {len(hits)} ; crashed()={q.crashed()} ---", flush=True)
    for h in hits[-50:]:
        print("   ", h, flush=True)

def main():
    q = RosQemu(NAME, ISO, disk=f"/tmp/{NAME}.img"); q.pid = PID

    print("=== install bootloader (MBR+VBR) ENTER ===", flush=True)
    q.send("ret"); time.sleep(3)
    q.screenshot("bootldr")

    print("=== finish_install -> reboot ===", flush=True)
    try:
        finish_install(q)
    except Exception as e:
        print(f"finish_install raised: {e!r}", flush=True)
    scan_fs(q, "post-install")

    print("=== first HD boot ===", flush=True)
    booted = boot_from_hd(q)
    print(f"boot_from_hd(first) -> {booted}", flush=True)
    q.screenshot("firstboot")
    if not booted:
        scan_fs(q, "firstboot-fail")
        # still try to screenshot to see the failure surface
        return

    print("=== first-boot wizard ===", flush=True)
    wiz = drive_first_boot_wizard(q)
    print(f"wizard -> {wiz}", flush=True)
    scan_fs(q, "post-wizard")

    print("=== second HD boot (installed desktop) ===", flush=True)
    if not q.is_alive():
        b2 = boot_from_hd(q); print(f"boot_from_hd(second) -> {b2}", flush=True)
    time.sleep(30)
    for i in range(4):
        p, _ = q.screenshot(f"desktop{i}"); print(f"  screenshot -> {p}", flush=True)
        time.sleep(8)
    scan_fs(q, "desktop")
    print("=== DONE; qemu left running ===", flush=True)

if __name__ == "__main__":
    main()
