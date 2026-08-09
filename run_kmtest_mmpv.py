#!/usr/bin/env python3
"""Boot the kmtest HD image (filtered to MmPageoutSysView via kmtestcd_setup.inf)
at -smp 4 (MP) + NIC, and harvest the MMPV: A/B result for the page-out-trimmer
system-space-view double-use repro.

  FIXED kernel  -> "[PASS] watch page survived trim" + "RESULT: FIXED"
  UNFIXED kernel-> "[FAIL] watch page survived trim ... 0=freed/BUG" + "RESULT: BUG"
                   (or a kdb:>/bugcheck during the trim itself)
"""
import os, sys, time, shutil, subprocess

SRC = "/home/kreijstal/git/reactos/build_nt10/kmtest.img"
DISK = "/tmp/kmtest_mmpv.img"
COM1 = "/tmp/kmtmmpv-com1.log"
COM2 = "/tmp/kmtmmpv-com2.log"
MON  = "/tmp/kmtmmpv-mon.sock"
TIMEOUT = 600
CRASH = ("kdb:>", "Entered debugger", "*** Fatal", "*** STOP", "released with rmaps", "Bad PTE")

def read(p):
    try:
        with open(p, "rb") as f: return f.read().decode("latin-1", "replace")
    except FileNotFoundError:
        return ""

def main():
    shutil.copyfile(SRC, DISK)
    for f in (COM1, COM2, MON):
        if os.path.exists(f): os.unlink(f)
    os.environ.setdefault("DISPLAY", ":10.0")
    cmd = ["qemu-system-x86_64", "-m", "2048", "-smp", "4",
           "-drive", f"file={DISK},format=raw,if=ide", "-boot", "c",
           "-device", "isa-debug-exit,iobase=0xf4,iosize=0x01",
           "-chardev", f"file,id=com1,path={COM1}",
           "-chardev", f"file,id=com2,path={COM2}",
           "-serial", "chardev:com1", "-serial", "chardev:com2",
           "-monitor", f"unix:{MON},server,nowait",
           "-netdev", "user,id=net0", "-device", "rtl8139,netdev=net0",
           "-display", "gtk", "-no-reboot", "-enable-kvm", "-s", "-daemonize"]
    print("=== launch (-smp 4) ===", flush=True)
    subprocess.run(cmd, check=True, capture_output=True)
    time.sleep(1)
    pid = None
    try:
        pid = int(subprocess.check_output(["pgrep", "-f", f"file={DISK}"], text=True).split()[0])
    except Exception:
        pass
    print(f"QEMU pid={pid}", flush=True)

    t0 = time.time()
    verdict = None
    while time.time() - t0 < TIMEOUT:
        blob = read(COM1)
        if "END_TEST(MmPageoutSysView)" in blob or "KMTCD-SUMMARY" in blob:
            verdict = "DONE"; print(f"  test completed at {int(time.time()-t0)}s", flush=True); break
        hit = next((s for l in blob.splitlines()[-60:] for s in CRASH if s in l), None)
        if hit:
            verdict = "CRASH"; print(f"  !! crash sig {hit!r} at {int(time.time()-t0)}s", flush=True); break
        if pid and not os.path.exists(f"/proc/{pid}"):
            verdict = "EXIT"; print(f"  QEMU exited at {int(time.time()-t0)}s", flush=True); break
        if int(time.time()-t0) % 15 == 0:
            print(f"  [{int(time.time()-t0)}s] waiting...", flush=True)
        time.sleep(3)
    if verdict is None:
        verdict = "TIMEOUT"

    blob = read(COM1)
    mmpv = [l.rstrip() for l in blob.splitlines() if "MMPV:" in l]
    print("\n--- MmPageoutSysView (MMPV) serial ---", flush=True)
    for l in mmpv: print("   ", l, flush=True)
    result = [l for l in mmpv if "RESULT:" in l]
    fails  = [l for l in mmpv if "[FAIL]" in l]
    print("\n=== VERDICT:", verdict, "| RESULT lines:", result or "<none>",
          "| FAILs:", len(fails), "===", flush=True)
    # quick crash context if any
    if verdict == "CRASH":
        for l in blob.splitlines()[-20:]: print("   |", l, flush=True)

    # leave qemu to exit on its own (isa-debug-exit); kill if still alive
    if pid and os.path.exists(f"/proc/{pid}"):
        try: subprocess.run(["kill", str(pid)])
        except Exception: pass

if __name__ == "__main__":
    main()
