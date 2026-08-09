#!/usr/bin/env python3
"""Drive a fresh NTFS install onto reactos.qcow2 using ros_test.py."""
import os, sys, time, traceback
sys.path.insert(0, "/home/kreijstal/git/reactos")
from ros_test import RosQemu, install_ntfs, monitor_copy, drive_first_boot_wizard

ISO  = "/home/kreijstal/git/reactos/build_nt10/bootcd.iso"
DISK = "/home/kreijstal/git/reactos/reactos.qcow2"

print(f"ISO  : {ISO}")
print(f"DISK : {DISK}")
sys.stdout.flush()

q = RosQemu(name="rosinstall", iso=ISO, disk=DISK)
q.start(boot="d")

try:
    install_ntfs(q)
    ok = monitor_copy(q, timeout_per_pct=240)
    if not ok:
        print("MONITOR_COPY: failed/stalled, aborting")
        sys.exit(2)

    print("=== Waiting for SETUP_PAGE:SUCCESS ===")
    q.wait("SETUP_PAGE:SUCCESS", 180)
    print("=== Pressing ENTER to reboot into GUI setup ===")
    q.send("ret")

    # After reboot, FreeLdr menu shows; -no-reboot kills the VM on reset.
    # qemu will exit when the guest issues a CPU reset. We then relaunch
    # boot=d so the bootcd's "second-stage" path (detects existing install)
    # transitions to GUI mode wizard.
    deadline = time.time() + 120
    while q.is_alive() and time.time() < deadline:
        time.sleep(1)
    if q.is_alive():
        print("guest did not reboot on its own; killing")
        q.kill()

    print("=== Relaunching for GUI setup phase ===")
    q.start(boot="d")
    ok = drive_first_boot_wizard(q, max_pages=20, page_timeout=60)
    if ok:
        print("=== INSTALL COMPLETE ===")
    else:
        print("=== GUI wizard incomplete ===")
        sys.exit(3)
except Exception:
    traceback.print_exc()
    sys.exit(1)
