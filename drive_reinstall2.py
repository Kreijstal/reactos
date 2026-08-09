#!/usr/bin/env python3
"""Continuation: from the 'Press ENTER to format' confirmation through copy,
finish, HD boot, first-boot wizard, and the installed desktop. Reattaches to
the already-running installer qemu (pid passed in)."""
import sys, time, os
from ros_test import (RosQemu, monitor_copy, finish_install,
                      boot_from_hd, drive_first_boot_wizard)

NAME = "nt10reinstall"
ISO = "build_nt10/bootcd.iso"
PID = int(sys.argv[1])

def scan_fs(q, tag):
    lines = q.log_lines()
    hits = []
    for l in lines:
        ll = l.lower()
        if ("ntfs" in ll or "fsctl" in ll or "fastfat" in ll) and \
           ("err" in ll or "fail" in ll or "assert" in ll or "corrupt" in ll):
            hits.append(l.strip())
        if "*** STOP" in l or "BugCheck" in l or "kdb:>" in l or "Assertion" in l:
            hits.append(l.strip())
    print(f"--- [{tag}] FS-error / crash lines: {len(hits)} ---", flush=True)
    for h in hits[-50:]:
        print("   ", h, flush=True)
    print(f"--- [{tag}] crashed()={q.crashed()} ---", flush=True)

def main():
    q = RosQemu(NAME, ISO, disk=f"/tmp/{NAME}.img")
    q.pid = PID

    print("=== confirm format (ENTER) ===", flush=True)
    q.send("ret"); time.sleep(2)

    print("=== monitor copy ===", flush=True)
    ok = monitor_copy(q)
    print(f"monitor_copy -> {ok}", flush=True)
    if not ok:
        for l in q.log_lines()[-30:]:
            print("   ", l.rstrip(), flush=True)
        scan_fs(q, "copy-fail")
        return

    print("=== finish install (-> reboot) ===", flush=True)
    finish_install(q)
    scan_fs(q, "post-install")

    print("=== first HD boot ===", flush=True)
    booted = boot_from_hd(q)
    print(f"boot_from_hd(first) -> {booted}", flush=True)
    q.screenshot("firstboot")
    if not booted:
        scan_fs(q, "firstboot-fail")
        return

    print("=== first-boot wizard ===", flush=True)
    wiz = drive_first_boot_wizard(q)
    print(f"wizard -> {wiz}", flush=True)
    scan_fs(q, "post-wizard")

    print("=== second HD boot (installed desktop) ===", flush=True)
    if not q.is_alive():
        b2 = boot_from_hd(q)
        print(f"boot_from_hd(second) -> {b2}", flush=True)
    time.sleep(30)
    for i in range(3):
        p, _ = q.screenshot(f"desktop{i}")
        print(f"  screenshot -> {p}", flush=True)
        time.sleep(8)
    scan_fs(q, "desktop")
    print("=== DONE; qemu left running ===", flush=True)

if __name__ == "__main__":
    main()
