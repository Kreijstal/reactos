#!/usr/bin/env python3
"""Reduced fastfat-corruption reproducer using a minimal C HTTP fetcher.

Replaces pacman+libalpm+msys2-runtime with /tmp/fatmimic.exe: raw Winsock
HTTP GET -> WriteFile to .part -> MoveFileExA to final -> reopen+read all.
This isolates whether the corruption needs libalpm semantics (hashing,
multi-file pipelining) or just network-paced WriteFile + rename + read-back.

Usage: scripts/repro_fatmimic.py <run_tag> [name1 name2 ...]
"""
import json, os, shutil, subprocess, sys, time, urllib.request
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ros_test import RosQemu

PRISTINE = "/tmp/fatpatch-test.img.pristine.localmirror"
USB      = "/tmp/fatpatch-test.img"
ISO      = "/home/kreijstal/git/reactos/build_nt62/bootcd.iso"
NAME     = "fatmimic"
WORK     = "/tmp/pacman_repro"
MIRDIR   = os.path.join(WORK, "_mirror")
MIMIC    = "/tmp/fatmimic.exe"

DEFAULT_NAMES = [
    "git-2.54.0-1-x86_64.pkg.tar.zst",
    "perl-5.42.2-1-x86_64.pkg.tar.zst",
    "vim-9.2.0452-1-x86_64.pkg.tar.zst",
]

def restore_and_inject():
    print(f"[fatmimic] restore {USB} from pristine", flush=True)
    if os.path.exists(USB): os.unlink(USB)
    r = subprocess.run(["cp", "--reflink=auto", PRISTINE, USB])
    if r.returncode != 0:
        shutil.copyfile(PRISTINE, USB)
    print(f"[fatmimic] mcopy {MIMIC} -> ::/fatmimic.exe", flush=True)
    subprocess.run(["mmd", "-i", f"{USB}@@1M", "::/stress"],
                   stderr=subprocess.DEVNULL)
    subprocess.run(["mcopy", "-i", f"{USB}@@1M", "-o", MIMIC, "::/fatmimic.exe"],
                   check=True)

def assert_mirror_up():
    try:
        with urllib.request.urlopen("http://127.0.0.1:18765/msys.db", timeout=3) as r:
            return r.status == 200
    except Exception as e:
        print(f"[fatmimic] MIRROR DOWN: {e}; start /tmp/serve_mirror.py first", flush=True)
        return False

def run_and_stream(c, label, cmd, *args, idle_s=120, deadline_s=600):
    last_recv = [time.time()]
    def stamp(prefix, p):
        last_recv[0] = time.time()
        sys.stdout.write(f"[{time.strftime('%H:%M:%S')}] {prefix}|{p.decode(errors='replace')}")
        sys.stdout.flush()
    print(f"\n[fatmimic] === {label}: {cmd} args={list(args)} ===", flush=True)
    c.spawn(cmd, *args, timeout_ms=deadline_s * 1000, idle_timeout_ms=900000)
    deadline = time.time() + deadline_s
    while True:
        now = time.time()
        if now > deadline: return ("deadline", None)
        if now - last_recv[0] >= idle_s:
            print(f"\n[fatmimic] {label}: idle {now - last_recv[0]:.0f}s", flush=True)
            return ("stall", None)
        try:
            status, info = c.stream(deadline=now + min(idle_s - (now - last_recv[0]) + 1, 30),
                                    on_stdout=lambda p: stamp("OUT", p),
                                    on_stderr=lambda p: stamp("ERR", p),
                                    echo=False)
        except RuntimeError as e:
            return ("ros_error", str(e))
        if status == "exit":
            print(f"\n[fatmimic] {label}: exited {info}", flush=True)
            return ("exit", info)
        if status == "eof": return ("eof", None)

def verify(image, names, run_tag):
    runwork = os.path.join(WORK, f"mimic_{run_tag}")
    os.makedirs(runwork, exist_ok=True)
    rows = []
    for name in names:
        local = os.path.join(runwork, name)
        subprocess.run(["mcopy", "-i", f"{image}@@1M", "-o",
                        f"::/stress/{name}", local],
                       stderr=subprocess.DEVNULL)
        if not os.path.exists(local):
            rows.append({"name": name, "status": "missing"}); continue
        mirror = os.path.join(MIRDIR, name)
        if not os.path.exists(mirror):
            rows.append({"name": name, "status": "no_mirror"}); continue
        a = open(local, "rb").read()
        b = open(mirror, "rb").read()
        if len(a) != len(b):
            rows.append({"name": name, "status": "size_differs",
                         "size": len(a), "mirror_size": len(b)})
        elif a == b:
            rows.append({"name": name, "status": "ok", "size": len(a)})
        else:
            j = 0
            while j < len(a) and a[j] == b[j]: j += 1
            rows.append({"name": name, "status": "diff",
                         "first_diff": j, "size": len(a)})
    return rows

def main():
    run_tag = sys.argv[1] if len(sys.argv) > 1 else f"r{int(time.time())}"
    names = sys.argv[2:] if len(sys.argv) > 2 else DEFAULT_NAMES
    print(f"[fatmimic] run_tag={run_tag} names={names}")

    if not os.path.exists(MIMIC):
        print(f"[fatmimic] {MIMIC} missing — build with x86_64-w64-mingw32-gcc")
        sys.exit(2)
    if not assert_mirror_up():
        sys.exit(2)

    restore_and_inject()
    q = RosQemu(NAME, iso=ISO)
    q.start_luagent(USB)
    q.wait_luagent_listen(timeout=180)
    c = q.luagent(hello_timeout=15)
    print("[fatmimic] luagent up", flush=True)

    args = ["C:\\fatmimic.exe", "10.0.2.2", "18765", "C:\\stress"] + list(names)
    s, info = run_and_stream(c, "fetch", "C:\\fatmimic.exe", *args,
                             idle_s=180, deadline_s=900)
    print(f"[fatmimic] result = {s}")

    try:
        c.spawn("C:\\usr\\bin\\sync.exe", "C:\\usr\\bin\\sync.exe",
                timeout_ms=30000, idle_timeout_ms=30000)
        for _ in range(20):
            st, _ = c.stream(deadline=time.time()+2,
                             on_stdout=lambda p: None,
                             on_stderr=lambda p: None, echo=False)
            if st == "exit": break
    except Exception:
        pass
    # Lazy-writer drain: bug2.md notes 20s + kill -9 isn't enough on a
    # ~24 MB write set. fatmimic builds without a guest sync.exe, and the
    # FAT volume flush path hung when tested directly. Extend wait to 120s.
    print("[fatmimic] sleeping 120s to let lazy-writer drain", flush=True)
    time.sleep(300)
    q.kill()

    post = f"/tmp/fatpatch-test.img.fatmimic_{run_tag}"
    shutil.copyfile(USB, post)
    rows = verify(post, names, run_tag)
    n_ok = sum(1 for r in rows if r["status"] == "ok")
    n_diff = sum(1 for r in rows if r["status"] == "diff")
    n_size = sum(1 for r in rows if r["status"] == "size_differs")
    n_other = sum(1 for r in rows if r["status"] not in ("ok","diff","size_differs"))
    print(f"\n[fatmimic] {run_tag}: ok={n_ok} byte_diff={n_diff} size_diff={n_size} other={n_other} total={len(rows)}")
    for r in rows:
        if r["status"] != "ok":
            print(f"  - {r}")
    out = os.path.join(WORK, f"fatmimic_{run_tag}.json")
    with open(out, "w") as f:
        json.dump({"run_tag": run_tag, "rows": rows}, f, indent=2)
    print(f"[fatmimic] image -> {post}, summary -> {out}")
    sys.exit(0 if (n_diff + n_size + n_other) == 0 else 2)

if __name__ == "__main__":
    main()
