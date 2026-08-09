#!/usr/bin/env python3
"""Boot ROS and run fatstress.exe via luagent — fast FAT corruption probe."""
import os, sys, time, shutil, subprocess
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ros_test import RosQemu

PRISTINE = "/tmp/fatpatch-test.img.pristine"
USB      = "/tmp/fatpatch-test.img"
ISO      = "/home/kreijstal/git/reactos/build_nt62/bootcd.iso"
NAME     = "fatstress"

OUT_DIR     = sys.argv[1] if len(sys.argv) > 1 else "C:\\stress"
N_FILES     = int(sys.argv[2]) if len(sys.argv) > 2 else 50
SIZE_BYTES  = int(sys.argv[3]) if len(sys.argv) > 3 else 5_000_000
VERIFY_EACH = sys.argv[4] if len(sys.argv) > 4 else "1"

# restore pristine each run
if os.path.exists(USB): os.unlink(USB)
shutil.copyfile(PRISTINE, USB)
print(f"[fatstress] restored pristine to {USB}")

# fatstress.exe was bundled into pristine separately; double-check by stat'ing
r = subprocess.run(["mdir", "-i", f"{USB}@@1M", "-b", "::/usr/bin/fatstress.exe"],
                   capture_output=True, text=True)
if "fatstress.exe" not in r.stdout.lower():
    print("[fatstress] bundling fatstress.exe into image...")
    subprocess.run(["mcopy", "-i", f"{USB}@@1M",
                    "/tmp/fatstress.exe", "::/usr/bin/fatstress.exe"], check=True)

q = RosQemu(NAME, iso=ISO)
q.start_luagent(USB)
q.wait_luagent_listen(timeout=180)
c = q.luagent(hello_timeout=15)
print("[fatstress] luagent up", flush=True)

last_recv = [time.time()]
last_payload = [b""]
def stamp(prefix, p):
    last_recv[0] = time.time()
    last_payload[0] = p
    sys.stdout.write(f"[{time.strftime('%H:%M:%S')}] {prefix}|{p.decode(errors='replace')}")
    sys.stdout.flush()

print(f"[fatstress] spawning fatstress.exe {OUT_DIR} {N_FILES} {SIZE_BYTES} verify={VERIFY_EACH}",
      flush=True)
c.spawn("C:\\usr\\bin\\fatstress.exe",
        "C:\\usr\\bin\\fatstress.exe", OUT_DIR, str(N_FILES), str(SIZE_BYTES),
        VERIFY_EACH,
        timeout_ms=900000, idle_timeout_ms=600000)

t0 = time.time(); deadline = t0 + 1800
while True:
    now = time.time()
    if now > deadline:
        print("[fatstress] hard deadline 30m"); break
    silent = now - last_recv[0]
    if silent > 600:
        print(f"\n[fatstress] STALL {silent:.0f}s; last={last_payload[0][-160:]!r}")
        break
    try:
        status, info = c.stream(deadline=now + 30,
                                on_stdout=lambda p: stamp("OUT", p),
                                on_stderr=lambda p: stamp("ERR", p),
                                echo=False)
    except RuntimeError as e:
        print(f"\n[fatstress] ROS error: {e}")
        break
    if status == "exit":
        print(f"\n[fatstress] exited: {info}")
        break
    if status == "eof":
        print("\n[fatstress] EOF"); break

print("[fatstress] saving image snapshot", flush=True)
post = "/tmp/fatpatch-test.img.fatstress"
try:
    c.spawn("C:\\usr\\bin\\sync.exe", "C:\\usr\\bin\\sync.exe",
            timeout_ms=30000, idle_timeout_ms=30000)
    for _ in range(15):
        st, _ = c.stream(deadline=time.time()+2,
                         on_stdout=lambda p: None,
                         on_stderr=lambda p: None, echo=False)
        if st == "exit": break
except Exception:
    pass
time.sleep(15)
q.kill()
shutil.copyfile(USB, post)
print(f"[fatstress] image saved -> {post}")
