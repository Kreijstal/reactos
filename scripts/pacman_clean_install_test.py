#!/usr/bin/env python3
"""End-to-end clean-install test for fastfat fix verification.

Boots ROS with /tmp/fatpatch-test.img (a freshly mformat'd FAT32 + msyshost
base) attached as USB, runs:
    1. pacman -Syu --noconfirm
    2. pacman -S --noconfirm git vim base-devel
streaming output with timestamps. Stalls trigger an idle dump.
"""
import os, sys, time, subprocess
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ros_test import RosQemu

ISO  = "/home/kreijstal/git/reactos/build_nt62/bootcd.iso"
USB  = "/tmp/fatpatch-test.img"
NAME = "pacclean"
IDLE_S = int(os.environ.get("IDLE_S", "180"))
HARD_DEADLINE = int(os.environ.get("HARD_DEADLINE", "5400"))  # 90 min

def _clean_db_lck(img):
    part = "/tmp/_part_clean_install.img"
    subprocess.run(["dd", f"if={img}", "bs=1M", "skip=1", f"of={part}",
                    "status=none"], check=True)
    subprocess.run(["mdel", "-i", part, "::/var/lib/pacman/db.lck"],
                   stderr=subprocess.DEVNULL)
    subprocess.run(["dd", f"if={part}", f"of={img}", "bs=1M", "seek=1",
                    "conv=notrunc", "status=none"], check=True)
    os.unlink(part)

_clean_db_lck(USB)
print(f"[harness] db.lck cleared on {USB}", flush=True)

q = RosQemu(NAME, iso=ISO)
q.start_luagent(USB)
print(f"[harness] waiting for luagent listen...", flush=True)
q.wait_luagent_listen(timeout=180)
print(f"[harness] luagent up, opening client", flush=True)
c = q.luagent(hello_timeout=15)

last_recv = time.time()
last_line_payload = b""

def _stamp(prefix, payload):
    global last_recv, last_line_payload
    last_recv = time.time()
    last_line_payload = payload
    sys.stdout.write(f"[{time.strftime('%H:%M:%S')}] {prefix}|{payload.decode(errors='replace')}")
    sys.stdout.flush()

def run_pacman(label, argv):
    """Spawn pacman with given argv, stream until exit/stall."""
    global last_recv, last_line_payload
    print(f"\n[harness] === {label}: {' '.join(argv)} ===", flush=True)
    last_recv = time.time()
    last_line_payload = b""
    c.spawn("C:\\usr\\bin\\pacman.exe", *argv,
            timeout_ms=HARD_DEADLINE * 1000, idle_timeout_ms=900000)
    t0 = time.time()
    deadline = t0 + HARD_DEADLINE
    while True:
        now = time.time()
        if now > deadline:
            print(f"[harness] {label}: hard deadline ({HARD_DEADLINE}s) exceeded", flush=True)
            return False
        silent_for = now - last_recv
        if silent_for >= IDLE_S:
            print(f"\n[harness] {label}: STALL ({silent_for:.0f}s); last bytes:", flush=True)
            print(f"  >>> {last_line_payload[-200:]!r}", flush=True)
            return False
        status, info = c.stream(deadline=now + min(IDLE_S - silent_for + 1, 30),
                                on_stdout=lambda p: _stamp("OUT", p),
                                on_stderr=lambda p: _stamp("ERR", p),
                                echo=False)
        if status == "exit":
            print(f"\n[harness] {label}: exited: {info}", flush=True)
            return True
        if status == "eof":
            print(f"\n[harness] {label}: EOF on connection", flush=True)
            return False

def drain_sync():
    """sync + sleep to flush fastfat lazy writer."""
    try:
        print("[harness] post-step drain: sync...", flush=True)
        c.spawn("C:\\usr\\bin\\sync.exe", "C:\\usr\\bin\\sync.exe",
                timeout_ms=30000, idle_timeout_ms=30000)
        for _ in range(20):
            s2, _ = c.stream(deadline=time.time()+2,
                             on_stdout=lambda p: None,
                             on_stderr=lambda p: None,
                             echo=False)
            if s2 == "exit":
                break
    except Exception as e:
        print(f"[harness] sync failed (ignored): {e}", flush=True)
    print("[harness] sleeping 25s for fastfat lazy flush...", flush=True)
    time.sleep(25)

# Step 1: pacman -Syu --noconfirm
ok1 = run_pacman("step1 -Syu",
                 ["C:\\usr\\bin\\pacman.exe", "-Syu", "--noconfirm"])
drain_sync()

# Step 2: pacman -S --noconfirm git vim base-devel
ok2 = False
if ok1:
    ok2 = run_pacman("step2 install",
                     ["C:\\usr\\bin\\pacman.exe", "-S", "--noconfirm",
                      "git", "vim", "base-devel"])
    drain_sync()
else:
    print("[harness] skipping step2 because step1 failed", flush=True)

print(f"[harness] killing qemu (step1={ok1} step2={ok2})", flush=True)
q.kill()
sys.exit(0 if (ok1 and ok2) else 1)
