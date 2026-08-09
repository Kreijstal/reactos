#!/usr/bin/env python3
"""Boot the writable kmtest HD image (FAT32 \\SystemRoot) and capture the
MmForceSectionClosed (MMFSC:) sub-test results, including the new system-space
view regression case. kmtcdrunner runs ONLY MmForceSectionClosed (targeted via
kmtestcd_setup.inf), logs to COM1, then fires IOCTL_KMT_EXIT_QEMU."""
import time
from ros_test import RosQemu

DISK = "/tmp/kmtest_mmfsc.img"

def main():
    q = RosQemu("kmtmmfsc", iso=None, disk=DISK)
    q.start(boot="c")
    print("=== kmtestimg booting from HD; waiting for runner ===", flush=True)
    deadline = time.time() + 360
    done = False
    while time.time() < deadline:
        if not q.is_alive():
            print("  QEMU exited (runner done)", flush=True); done = True; break
        if any("END_TEST(MmForceSectionClosed)" in l for l in q.log_lines()):
            print("  saw END_TEST(MmForceSectionClosed)", flush=True); time.sleep(2); done = True; break
        time.sleep(5)
    if not done:
        print("  TIMEOUT waiting for test", flush=True)

    lines = q.log_lines()
    mmfsc = [l.rstrip().strip() for l in lines if "MMFSC:" in l]
    print("--- MmForceSectionClosed serial output ---", flush=True)
    for l in mmfsc:
        print("   ", l, flush=True)
    fails = [l for l in mmfsc if "[FAIL]" in l]
    passes = [l for l in mmfsc if "[PASS]" in l]
    sysview = [l for l in mmfsc if "SystemView" in l.replace(" ", "") or "SYSTEMview" in l.upper() or "system-space" in l or "SYSTEM view" in l]
    print(f"=== RESULT: PASS={len(passes)} FAIL={len(fails)} (system-view-related lines={len(sysview)}) ===", flush=True)
    print("=== DONE ===", flush=True)

if __name__ == "__main__":
    main()
