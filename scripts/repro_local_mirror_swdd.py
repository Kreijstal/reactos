#!/usr/bin/env python3
"""Stable corruption reproducer using a LOCAL HTTP msys2 mirror.

Restores /tmp/fatpatch-test.img from /tmp/fatpatch-test.img.pristine (must
already have pacman.conf pointing at http://10.0.2.2:18765/), then runs
pacman -Syu and pacman -S of a configurable package list. Fast because
all packages come from local mirror at LAN speed.

Pre-req: /tmp/serve_mirror.py running on host port 18765.

Usage: scripts/repro_local_mirror.py <run_tag> [pkg1 pkg2 ...]
"""
import json, os, shutil, subprocess, sys, time, urllib.request
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ros_test import RosQemu

PRISTINE = "/tmp/fatpatch-test.img.pristine.localmirror"
USB      = "/tmp/fatpatch-test.img"
ISO      = "/home/kreijstal/git/reactos/build_nt62/bootcd.iso"
NAME     = "pacmirror"
WORK     = "/tmp/pacman_repro"
MIRDIR   = os.path.join(WORK, "_mirror")

DEFAULT_PKGS = ["git", "vim", "perl", "libgnutls", "libopenssl",
                "msys2-runtime", "gettext", "xz", "openssl",
                "ca-certificates", "libcurl", "curl", "libssh2",
                "libnghttp2", "libidn2", "libpsl", "libtasn1",
                "libnettle", "libhogweed", "libgcrypt", "libgpg-error",
                "p11-kit", "libp11-kit", "libzstd", "zstd", "zlib",
                "bzip2"]

def restore_pristine():
    print(f"[repro] restore {USB} from pristine", flush=True)
    if os.path.exists(USB): os.unlink(USB)
    r = subprocess.run(["cp", "--reflink=auto", PRISTINE, USB])
    if r.returncode != 0:
        shutil.copyfile(PRISTINE, USB)

def assert_mirror_up():
    """Sanity: confirm mirror is reachable from host before booting ROS."""
    try:
        with urllib.request.urlopen("http://127.0.0.1:18765/msys.db", timeout=3) as r:
            return r.status == 200
    except Exception as e:
        print(f"[repro] MIRROR DOWN: {e}; start /tmp/serve_mirror.py first", flush=True)
        return False

def run_pacman(c, label, argv, idle_s=600, deadline_s=3600):
    last_recv = [time.time()]
    last_payload = [b""]
    def stamp(prefix, p):
        last_recv[0] = time.time()
        last_payload[0] = p
        sys.stdout.write(f"[{time.strftime('%H:%M:%S')}] {prefix}|{p.decode(errors='replace')}")
        sys.stdout.flush()
    print(f"\n[repro] === {label}: {' '.join(argv)} ===", flush=True)
    c.spawn("C:\\usr\\bin\\pacman.exe", *argv,
            timeout_ms=deadline_s * 1000, idle_timeout_ms=900000)
    deadline = time.time() + deadline_s
    while True:
        now = time.time()
        if now > deadline: return ("deadline", None)
        silent = now - last_recv[0]
        if silent >= idle_s:
            print(f"\n[repro] {label}: STALL {silent:.0f}s; last={last_payload[0][-160:]!r}", flush=True)
            return ("stall", last_payload[0])
        try:
            status, info = c.stream(deadline=now + min(idle_s - silent + 1, 30),
                                    on_stdout=lambda p: stamp("OUT", p),
                                    on_stderr=lambda p: stamp("ERR", p),
                                    echo=False)
        except RuntimeError as e:
            msg = str(e)
            if "STATUS_FILE_CORRUPT_ERROR" in msg or "Assertion failed" in msg:
                print(f"\n[repro] {label}: ROS ASSERT during recv -> bug fired", flush=True)
                return ("ros_assert", msg)
            return ("ros_error", msg)
        if status == "exit":
            print(f"\n[repro] {label}: exited {info}", flush=True)
            return ("exit", info)
        if status == "eof": return ("eof", None)

def verify_cache(image, run_tag):
    runwork = os.path.join(WORK, f"run_{run_tag}")
    os.makedirs(runwork, exist_ok=True)
    out = subprocess.check_output(
        ["mdir", "-i", f"{image}@@1M", "-b", "::/var/cache/pacman/pkg/"],
        text=True, stderr=subprocess.DEVNULL)
    files = sorted(set(
        os.path.basename(l.strip())
        for l in out.splitlines()
        if l.strip().endswith(".zst")))
    rows = []
    for name in files:
        local = os.path.join(runwork, name)
        subprocess.run(["mcopy", "-i", f"{image}@@1M", "-o",
                        f"::/var/cache/pacman/pkg/{name}", local],
                       stderr=subprocess.DEVNULL)
        if not os.path.exists(local): continue
        mirror = os.path.join(MIRDIR, name)
        if not os.path.exists(mirror) or os.path.getsize(mirror) == 0:
            try:
                urllib.request.urlretrieve(f"http://127.0.0.1:18765/{name}", mirror)
            except Exception:
                rows.append({"name": name, "status": "no_mirror"})
                continue
        a = open(local, "rb").read()
        b = open(mirror, "rb").read()
        if len(a) != len(b):
            rows.append({"name": name, "status": "size_differs",
                         "size": len(a), "mirror_size": len(b)})
        elif a == b:
            rows.append({"name": name, "status": "ok"})
        else:
            j = 0
            while j < len(a) and a[j] == b[j]: j += 1
            rows.append({"name": name, "status": "diff",
                         "first_diff": j, "size": len(a)})
    return rows

def main():
    run_tag = sys.argv[1] if len(sys.argv) > 1 else f"r{int(time.time())}"
    pkgs = sys.argv[2:] if len(sys.argv) > 2 else DEFAULT_PKGS
    print(f"[repro] run_tag={run_tag} pkgs={pkgs}")

    if not assert_mirror_up():
        sys.exit(2)

    restore_pristine()
    q = RosQemu(NAME, iso=ISO)
    q.start_luagent(USB)
    q.wait_luagent_listen(timeout=180)
    c = q.luagent(hello_timeout=15)
    print("[repro] luagent up", flush=True)

    s1, _ = run_pacman(c, "step1 -Syu",
                       ["C:\\usr\\bin\\pacman.exe", "-Syu", "--noconfirm"],
                       idle_s=120, deadline_s=300)
    if s1 != "exit":
        print(f"[repro] step1 didn't exit cleanly: {s1}; bailing", flush=True)
        q.kill()
        sys.exit(1)

    argv = ["C:\\usr\\bin\\pacman.exe", "-Swdd", "--noconfirm"] + list(pkgs)
    s2, info = run_pacman(c, "step2 download", argv,
                          idle_s=300, deadline_s=3600)
    print(f"[repro] step2 result = {s2}")

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
    time.sleep(20)
    q.kill()

    post = f"/tmp/fatpatch-test.img.localmirror_{run_tag}"
    shutil.copyfile(USB, post)
    rows = verify_cache(post, run_tag)
    n_ok = sum(1 for r in rows if r["status"] == "ok")
    n_diff = sum(1 for r in rows if r["status"] == "diff")
    n_size = sum(1 for r in rows if r["status"] == "size_differs")
    n_other = sum(1 for r in rows if r["status"] not in ("ok","diff","size_differs"))
    print(f"\n[repro] {run_tag}: ok={n_ok} byte_diff={n_diff} size_diff={n_size} other={n_other} total={len(rows)}")
    if n_diff or n_size:
        print("[repro] CORRUPT FILES:")
        for r in rows:
            if r["status"] in ("diff","size_differs"):
                print(f"  - {r}")
    out = os.path.join(WORK, f"localmirror_{run_tag}.json")
    summary = {"run_tag": run_tag, "ok": n_ok, "byte_diff": n_diff,
               "size_diff": n_size, "other": n_other, "total": len(rows),
               "corrupt_files": [r for r in rows if r["status"] != "ok"]}
    with open(out, "w") as f:
        json.dump(summary, f, indent=2)
    print(f"[repro] image -> {post}, summary -> {out}")
    sys.exit(0 if (n_diff + n_size + n_other) == 0 else 2)

if __name__ == "__main__":
    main()
