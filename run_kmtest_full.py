#!/usr/bin/env python3
"""Boot the writable kmtest HD image (nt10 amd64) and run the FULL kmtest
suite via kmtcdrunner (empty filter = run every test in kmtest_.exe --list).

kmtcdrunner emits structured markers:
    KMTCD-RUNNER start ...
    KMTCD-BEGIN <name>
    KMTCD-END   <name> exit=N failures=M
    KMTCD-SUMMARY tests_run=X tests_failed=Y errored=Z assert_failures=W
It tries to write them to COM2, but on this boot COM2 open fails in the guest
(serial=0); every EmitLine ALSO does OutputDebugStringA, so the markers land on
COM1 (the kernel debug log) regardless. We therefore harvest markers from BOTH
serial logs. When all tests have run it fires IOCTL_KMT_EXIT_QEMU -> isa-debug-
exit port 0xF4 -> QEMU terminates.

COM1 -> kernel debug log + OutputDebugStringA markers -> {name}-com1.log
COM2 -> kmtcdrunner serial markers (if COM2 opened)    -> {name}-com2.log
"""
import os
import re
import sys
import time
import argparse
import shutil
import subprocess

SRC_IMG = "/home/kreijstal/git/reactos/build_nt10/kmtest.img"
DISK = "/tmp/kmtest_full.img"
NAME = "kmtfull"
COM1 = f"/tmp/{NAME}-com1.log"
COM2 = f"/tmp/{NAME}-com2.log"
MON = f"/tmp/{NAME}-mon.sock"
TIMEOUT = 1800  # 30 min hard cap for the whole suite

CRASH_SIGS = ("kdb:>", "Entered debugger", "*** Fatal System Error", "*** STOP:")

# A writable NTFS data disk, attached as a 2nd IDE drive so NtfsDirIndex (which
# scans D:-Z: for an "NTFS" volume) has something to run against; without it the
# test fails with "No writable NTFS volume found".  MBR-partitioned (partmgr
# won't enumerate a bare-disk NTFS), one type-0x07 partition formatted by the
# host mkntfs.  Created once and reused across runs.
NTFS_DATA = "/tmp/kmtest_ntfsdata.img"
CONTROL_DISK = "/tmp/kmtest_filter.img"


def parse_args():
    parser = argparse.ArgumentParser(description="Run the ReactOS kmtest image under QEMU")
    parser.add_argument("--filter", default=None,
                        help="kmtcdrunner filter, for example NtfsDirIndex or !ExHardError")
    parser.add_argument("--name", default=NAME,
                        help="prefix for /tmp logs and copied boot disk")
    parser.add_argument("--gdb-port", default="1236",
                        help="QEMU gdbstub TCP port")
    parser.add_argument("--timeout", type=int, default=TIMEOUT,
                        help="timeout in seconds")
    parser.add_argument("--usb-image", default=None,
                        help="optional raw USB mass-storage image to attach through qemu-xhci")
    return parser.parse_args()


def make_ntfs_data_disk():
    if os.path.exists(NTFS_DATA):
        return
    print(f"=== creating NTFS data disk {NTFS_DATA} ===", flush=True)
    part = NTFS_DATA + ".part"
    subprocess.run(["truncate", "-s", "256M", NTFS_DATA], check=True)
    subprocess.run(["sfdisk", NTFS_DATA], check=True,
                   input=b"label: dos\nstart=2048, size=522240, type=7, bootable\n",
                   capture_output=True)
    subprocess.run(["truncate", "-s", str(522240 * 512), part], check=True)
    subprocess.run(["mkntfs", "-F", "-Q", "-L", "KMTNTFS", part], check=True,
                   capture_output=True)
    subprocess.run(["dd", f"if={part}", f"of={NTFS_DATA}", "bs=512", "seek=2048",
                    "conv=notrunc"], check=True, capture_output=True)
    os.unlink(part)


def make_control_disk(filter_name):
    if os.path.exists(CONTROL_DISK):
        os.unlink(CONTROL_DISK)
    if filter_name is None:
        return None

    selection = "/tmp/KMTEST.SEL"
    subprocess.run(["truncate", "-s", "1440K", CONTROL_DISK], check=True)
    subprocess.run(["mformat", "-i", CONTROL_DISK, "::"], check=True)
    with open(selection, "w", encoding="ascii") as f:
        f.write(filter_name + "\n")
    try:
        subprocess.run(["mcopy", "-i", CONTROL_DISK, selection, "::KMTEST.SEL"],
                       check=True)
    finally:
        if os.path.exists(selection):
            os.unlink(selection)
    return CONTROL_DISK


def read(path):
    try:
        with open(path, "rb") as f:
            return f.read().decode("latin-1", "replace")
    except FileNotFoundError:
        return ""


def markers():
    """All KMTCD-* marker lines from both serial logs, in first-seen order."""
    seen = []
    s = set()
    for blob in (read(COM1), read(COM2)):
        for line in blob.splitlines():
            line = line.strip()
            i = line.find("KMTCD-")
            if i >= 0:
                m = line[i:]
                if m not in s:
                    s.add(m)
                    seen.append(m)
    return seen


def main():
    global DISK, NAME, COM1, COM2, MON, TIMEOUT

    args = parse_args()
    NAME = args.name
    DISK = f"/tmp/{NAME}.img"
    COM1 = f"/tmp/{NAME}-com1.log"
    COM2 = f"/tmp/{NAME}-com2.log"
    MON = f"/tmp/{NAME}-mon.sock"
    TIMEOUT = args.timeout

    if not os.path.exists(SRC_IMG):
        print(f"FATAL: {SRC_IMG} not found (build kmtestimg first)", flush=True)
        sys.exit(2)

    print(f"=== copying {SRC_IMG} -> {DISK} ===", flush=True)
    shutil.copyfile(SRC_IMG, DISK)
    make_ntfs_data_disk()
    control_disk = make_control_disk(args.filter)
    usb_overlay = None
    if args.usb_image is not None:
        usb_overlay = f"/tmp/{NAME}-usb.qcow2"
        if os.path.exists(usb_overlay):
            os.unlink(usb_overlay)
        subprocess.run([
            "qemu-img", "create", "-f", "qcow2",
            "-F", "raw", "-b", args.usb_image, usb_overlay
        ], check=True, capture_output=True)
    for f in (COM1, COM2, MON):
        if os.path.exists(f):
            os.unlink(f)

    cmd = [
        "qemu-system-x86_64", "-m", "4096",
        "-drive", f"file={DISK},format=raw,if=ide",
        "-drive", f"file={NTFS_DATA},format=raw,if=ide",
        "-boot", "c",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x01",
        "-chardev", f"file,id=com1,path={COM1}",
        "-chardev", f"file,id=com2,path={COM2}",
        "-serial", "chardev:com1",
        "-serial", "chardev:com2",
        "-monitor", f"unix:{MON},server,nowait",
        # rtl8139 + user netdev so tcpip.sys binds an adapter (TcpIpTdi /
        # TcpIpConnect need TDI address objects + a working loopback path),
        # 4 GB RAM so the _WIN64 2 GB paged-pool ExPools allocation can succeed.
        "-netdev", "user,id=net0",
        "-device", "rtl8139,netdev=net0",
        "-display", "gtk",
        "-no-reboot", "-enable-kvm", "-gdb", f"tcp::{args.gdb_port}", "-daemonize",
    ]
    if args.usb_image is not None:
        boot_arg = cmd.index("-boot")
        cmd[boot_arg:boot_arg] = [
            "-device", "qemu-xhci,id=xhci",
            "-drive", f"if=none,id=msysusb,file={usb_overlay},format=qcow2",
            "-device", "usb-storage,bus=xhci.0,drive=msysusb",
        ]
    if control_disk is not None:
        boot_arg = cmd.index("-boot")
        cmd[boot_arg:boot_arg] = ["-drive", f"file={control_disk},format=raw,if=ide"]
    print("=== launching QEMU ===\n  " + " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, capture_output=True)

    time.sleep(1)
    pid = None
    try:
        out = subprocess.check_output(["pgrep", "-f", f"file={DISK}"], text=True)
        pid = int(out.split()[0])
    except Exception:
        pass
    print(f"QEMU pid={pid}", flush=True)

    start = time.time()
    deadline = start + TIMEOUT
    summary = None
    last_report = 0
    while time.time() < deadline:
        alive = pid is not None and os.path.exists(f"/proc/{pid}")
        mk = markers()
        begins = [m for m in mk if m.startswith("KMTCD-BEGIN")]
        ends = [m for m in mk if m.startswith("KMTCD-END")]
        summary = next((m for m in mk if m.startswith("KMTCD-SUMMARY")), summary)
        cur = begins[-1] if begins else None

        now = time.time()
        if now - last_report > 15:
            print(f"  [{int(now-start)}s] alive={alive} begun={len(begins)} "
                  f"ended={len(ends)} cur={cur}", flush=True)
            last_report = now

        if summary:
            print(f"  saw {summary}", flush=True)
            time.sleep(1)
            break
        if not alive:
            print("  QEMU exited (isa-debug-exit fired)", flush=True)
            time.sleep(1)
            break
        c1 = read(COM1).splitlines()
        hit = next((s for l in c1[-50:] for s in CRASH_SIGS if s in l), None)
        if hit:
            print(f"  !! COM1 crash/KDB signature: {hit!r}", flush=True)
            print(f"  !! crashing test (last BEGIN): {cur}", flush=True)
            for l in c1[-25:]:
                print("    " + l, flush=True)
            break
        time.sleep(5)

    # Final tally from markers
    mk = markers()
    end_re = re.compile(r"KMTCD-END\s+(\S+)\s+exit=(\d+)\s+failures=(-?\d+)")
    passed = failed = errored = 0
    fail_names, err_names = [], []
    end_lines = [m for m in mk if m.startswith("KMTCD-END")]
    for m in end_lines:
        mo = end_re.search(m)
        if not mo:
            continue
        name, exitc, fails = mo.group(1), int(mo.group(2)), int(mo.group(3))
        if exitc == 0 and fails == 0:
            passed += 1
        else:
            if fails > 0:
                failed += 1
                fail_names.append(f"{name}(f={fails})")
            if exitc != 0:
                errored += 1
                err_names.append(f"{name}(exit={exitc})")

    print("\n================ KMTEST RESULT (nt10 amd64) ================", flush=True)
    print(f"tests with END markers : {len(end_lines)}", flush=True)
    print(f"PASSED (exit=0,fail=0) : {passed}", flush=True)
    print(f"FAILED (>=1 assert)    : {failed}", flush=True)
    if fail_names:
        print(f"    {fail_names}", flush=True)
    print(f"ERRORED (exit!=0)      : {errored}", flush=True)
    if err_names:
        print(f"    {err_names}", flush=True)
    if summary:
        print(f"runner summary         : {summary}", flush=True)
    else:
        print("runner summary         : <none — run did not complete cleanly>", flush=True)
    print("===========================================================", flush=True)


if __name__ == "__main__":
    main()
