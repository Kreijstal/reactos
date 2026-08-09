#!/usr/bin/env python3
"""Headless SMP variant of run_kmtest_full.py.

Boots build_nt10/kmtest.img with -smp N and the MBR-wrapped NTFS data disk so
NtfsDirIndex/FatPerf have a writable NTFS volume, runs the FULL kmtest suite via
kmtcdrunner, harvests KMTCD-* markers from both serial logs, and tallies.

Differences from run_kmtest_full.py:
  * -smp N (default 4) to exercise the MP scheduler / NTFS B-Tree under SMP
  * -display none (this host is headless; gtk fails)
"""
import os, re, sys, time, shutil, subprocess

SMP = int(os.environ.get("KMT_SMP", "4"))
SRC_IMG = "/home/kreijstal/git/reactos/build_nt10/kmtest.img"
DISK = "/tmp/kmtest_smp.img"
NAME = "kmtsmp"
COM1 = f"/tmp/{NAME}-com1.log"
COM2 = f"/tmp/{NAME}-com2.log"
MON = f"/tmp/{NAME}-mon.sock"
TIMEOUT = int(os.environ.get("KMT_TIMEOUT", "2400"))
NTFS_DATA = "/tmp/kmtest_ntfsdata.img"
CRASH_SIGS = ("kdb:>", "Entered debugger", "*** Fatal System Error", "*** STOP:",
              "*** Assertion failed")


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


def read(path):
    try:
        with open(path, "rb") as f:
            return f.read().decode("latin-1", "replace")
    except FileNotFoundError:
        return ""


def markers():
    seen, s = [], set()
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
    if not os.path.exists(SRC_IMG):
        print(f"FATAL: {SRC_IMG} not found", flush=True)
        sys.exit(2)
    print(f"=== copying {SRC_IMG} -> {DISK} (smp={SMP}) ===", flush=True)
    shutil.copyfile(SRC_IMG, DISK)
    make_ntfs_data_disk()
    for f in (COM1, COM2, MON):
        if os.path.exists(f):
            os.unlink(f)
    cmd = [
        "qemu-system-x86_64", "-m", "4096", "-smp", str(SMP),
        "-drive", f"file={DISK},format=raw,if=ide",
        "-drive", f"file={NTFS_DATA},format=raw,if=ide",
        "-boot", "c",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x01",
        "-chardev", f"file,id=com1,path={COM1}",
        "-chardev", f"file,id=com2,path={COM2}",
        "-serial", "chardev:com1",
        "-serial", "chardev:com2",
        "-monitor", f"unix:{MON},server,nowait",
        "-netdev", "user,id=net0",
        "-device", "rtl8139,netdev=net0",
        "-display", "none",
        "-no-reboot", "-enable-kvm", "-daemonize",
    ]
    print("=== launching QEMU ===\n  " + " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, capture_output=True)
    time.sleep(1)
    pid = None
    try:
        pid = int(subprocess.check_output(["pgrep", "-f", f"file={DISK}"], text=True).split()[0])
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
            for l in c1[-30:]:
                print("    " + l, flush=True)
            break
        time.sleep(5)
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
    print(f"\n============ KMTEST RESULT (nt10 amd64, smp={SMP}) ============", flush=True)
    print(f"tests with END markers : {len(end_lines)}", flush=True)
    print(f"PASSED (exit=0,fail=0) : {passed}", flush=True)
    print(f"FAILED (>=1 assert)    : {failed}", flush=True)
    if fail_names:
        print(f"    {fail_names}", flush=True)
    print(f"ERRORED (exit!=0)      : {errored}", flush=True)
    if err_names:
        print(f"    {err_names}", flush=True)
    print(f"runner summary         : {summary or '<none — incomplete>'}", flush=True)
    print("==============================================================", flush=True)


if __name__ == "__main__":
    main()
