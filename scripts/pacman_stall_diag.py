#!/usr/bin/env python3
"""Pacman download stall diagnosis: boot ROS, start luagent, run pacman -Sw bash
   --debug --noconfirm, stream output and timestamps every line.  When idle for
   N seconds, dump the last lines and exit so we can see exactly where pacman
   blocked."""
import os, sys, time, signal, subprocess
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ros_test import RosQemu

def _clean_db_lck(img):
    """Strip any leftover /var/lib/pacman/db.lck via mdel on a temp partition
       extract, so pacman can run.  Required because kill -9 may have left
       a stale lock on disk from a prior run."""
    part = "/tmp/_part_clean.img"
    subprocess.run(["dd", f"if={img}", "bs=1M", "skip=1", f"of={part}",
                    "status=none"], check=True)
    subprocess.run(["mdel", "-i", part, "::/var/lib/pacman/db.lck"],
                   stderr=subprocess.DEVNULL)
    subprocess.run(["dd", f"if={part}", f"of={img}", "bs=1M", "seek=1",
                    "conv=notrunc", "status=none"], check=True)
    os.unlink(part)

ISO  = "/home/kreijstal/git/reactos/build_nt62/bootcd.iso"
USB  = "/tmp/fatpatch-test.img"
NAME = "pacstall"

# Pull a fresh download (10-byte zapped placeholder forces re-download via Range)
# Accept multiple packages: "bash filesystem gzip"
PKGS  = sys.argv[1].split() if len(sys.argv) > 1 else ["bash"]
IDLE_S = int(sys.argv[2]) if len(sys.argv) > 2 else 90

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

DEBUG = bool(int(os.environ.get("PAC_DEBUG", "0")))
argv = ["C:\\usr\\bin\\pacman.exe", "-Sw", "--noconfirm"]
if DEBUG: argv.append("--debug")
argv += PKGS
c.spawn("C:\\usr\\bin\\pacman.exe", *argv,
        timeout_ms=1800000, idle_timeout_ms=900000)

deadline = time.time() + 1800
last_stall_print = time.time()
while True:
    now = time.time()
    if now > deadline:
        print(f"[harness] hard deadline; killing", flush=True)
        break
    silent_for = now - last_recv
    if silent_for >= IDLE_S:
        print(f"\n[harness] STALL DETECTED: silent for {silent_for:.0f}s; last bytes were:", flush=True)
        print(f"  >>> {last_line_payload[-200:]!r}", flush=True)
        break
    # Re-arm: stream(...) returns on each frame; we want lines as they come
    status, info = c.stream(deadline=now + min(IDLE_S - silent_for + 1, 30),
                            on_stdout=lambda p: _stamp("OUT", p),
                            on_stderr=lambda p: _stamp("ERR", p),
                            echo=False)
    if status == "exit":
        print(f"[harness] pacman exited: {info}", flush=True)
        # Drain cache: run `sync` in the guest, then sleep so async writes flush
        try:
            print("[harness] post-exit drain: spawning sync...", flush=True)
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
        print("[harness] sleeping 25s for fastfat lazy writer flush...", flush=True)
        time.sleep(25)
        break
    if status == "eof":
        print(f"[harness] connection EOF", flush=True)
        break
    # status=="timeout" → loop and re-check stall

print(f"[harness] killing qemu", flush=True)
q.kill()
