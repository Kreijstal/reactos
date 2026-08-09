#!/usr/bin/env python3
"""Single-file synthetic-write reproducer for the Cc per-cycle flush bottleneck.

Boots ROS livecd, runs /tmp/bigwrite.exe to write <size_MiB> of a deterministic
page-index pattern to a single file on the FAT-formatted USB-MSC volume.
After the writer exits and prints in-process verify mismatches=0, the harness
drains, kills QEMU, and reads the raw FAT image. Each 4 KiB page on disk is
checked: page N's first 4 bytes should equal N (uint32 LE). Mismatch = a
cluster the lazy writer never flushed (still holds pristine-image bytes).

Two-file fatmimic already reproduces the bug. This script isolates whether
the bug needs multiple VACBs (fails with 2 SharedCacheMaps, would pass with 1)
or just sufficient queue depth (fails with 1 large file too).

Usage: scripts/repro_bigwrite.py <run_tag> [size_MiB]
"""
import os, shutil, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ros_test import RosQemu

PRISTINE = "/tmp/fatpatch-test.img.pristine.localmirror"
USB      = "/tmp/fatpatch-test.img"
ISO      = "/home/kreijstal/git/reactos/build_nt62/bootcd.iso"
NAME     = "bigwrite"
WORK     = "/tmp/pacman_repro"
BIG      = "/tmp/bigwrite.exe"

def restore_and_inject():
    print(f"[bigwrite] restore {USB} from pristine", flush=True)
    if os.path.exists(USB): os.unlink(USB)
    r = subprocess.run(["cp", "--reflink=auto", PRISTINE, USB])
    if r.returncode != 0:
        shutil.copyfile(PRISTINE, USB)
    subprocess.run(["mmd", "-i", f"{USB}@@1M", "::/big"],
                   stderr=subprocess.DEVNULL)
    subprocess.run(["mcopy", "-i", f"{USB}@@1M", "-o", BIG, "::/bigwrite.exe"],
                   check=True)

def verify(image, mb, run_tag):
    runwork = os.path.join(WORK, f"bigwrite_{run_tag}")
    os.makedirs(runwork, exist_ok=True)
    local = os.path.join(runwork, "test.bin")
    if os.path.exists(local): os.unlink(local)
    subprocess.run(["mcopy", "-i", f"{image}@@1M", "-o",
                    "::/big/test.bin", local],
                   stderr=subprocess.DEVNULL)
    if not os.path.exists(local):
        return {"status": "missing"}
    expected_size = mb * 1024 * 1024
    actual_size = os.path.getsize(local)
    out = {"size": actual_size, "expected": expected_size}
    if actual_size < expected_size:
        out["status"] = "size_differs"
    pages_total = actual_size // 4096
    bad_first = -1
    bad_count = 0
    with open(local, "rb") as f:
        for page_idx in range(pages_total):
            chunk = f.read(4096)
            if len(chunk) != 4096: break
            actual = int.from_bytes(chunk[0:4], "little")
            if actual != page_idx:
                if bad_first < 0: bad_first = page_idx
                bad_count += 1
    out["pages_checked"] = pages_total
    out["bad_pages"] = bad_count
    if bad_first >= 0:
        out["first_bad_page"] = bad_first
        out["first_bad_offset"] = bad_first * 4096
        if "status" not in out: out["status"] = "diff"
    if "status" not in out: out["status"] = "ok"
    return out

def main():
    tag = sys.argv[1] if len(sys.argv) > 1 else f"r{int(time.time())}"
    mb = int(sys.argv[2]) if len(sys.argv) > 2 else 25
    if not os.path.exists(BIG):
        print(f"[bigwrite] {BIG} missing — build with x86_64-w64-mingw32-gcc")
        sys.exit(2)

    restore_and_inject()
    q = RosQemu(NAME, iso=ISO)
    q.start_luagent(USB)
    q.wait_luagent_listen(timeout=180)
    c = q.luagent(hello_timeout=15)
    print(f"[bigwrite] luagent up, writing {mb} MiB to ::/big/test.bin", flush=True)

    last_recv = [time.time()]
    def on_out(p):
        last_recv[0] = time.time()
        sys.stdout.write(p.decode(errors='replace'))
        sys.stdout.flush()
    def on_err(p):
        last_recv[0] = time.time()
        sys.stderr.write(p.decode(errors='replace'))
        sys.stderr.flush()
    c.spawn("C:\\bigwrite.exe", "C:\\bigwrite.exe",
            "C:\\big\\test.bin", str(mb),
            timeout_ms=900000, idle_timeout_ms=900000)
    deadline = time.time() + 900
    while True:
        now = time.time()
        if now > deadline:
            print("\n[bigwrite] deadline; bailing", flush=True); break
        if now - last_recv[0] > 180:
            print(f"\n[bigwrite] idle {now-last_recv[0]:.0f}s; assuming stuck", flush=True); break
        try:
            st, info = c.stream(deadline=now + 30,
                                on_stdout=on_out, on_stderr=on_err, echo=False)
        except RuntimeError as e:
            print(f"\n[bigwrite] ros_error: {e}", flush=True); break
        if st == "exit":
            print(f"\n[bigwrite] exited {info}", flush=True); break
        if st == "eof":
            print(f"\n[bigwrite] eof", flush=True); break

    # Lazy-writer drain: deliberately no sync.exe, want to test the lazy
    # writer alone end-to-end.
    print("[bigwrite] sleeping 300s for lazy-writer drain", flush=True)
    time.sleep(300)
    q.kill()

    post = f"/tmp/fatpatch-test.img.bigwrite_{tag}"
    shutil.copyfile(USB, post)
    r = verify(post, mb, tag)
    print(f"\n[bigwrite] tag={tag} mb={mb}: {r}")
    print(f"[bigwrite] image -> {post}")
    sys.exit(0 if r["status"] == "ok" else 2)

if __name__ == "__main__":
    main()
