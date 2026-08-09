#!/usr/bin/env python3
"""Stable reproducer for the fastfat / Cc page-cache aliasing corruption bug
exposed by heavy pacman -S workloads.

Each invocation:
  1. Restores /tmp/fatpatch-test.img from /tmp/fatpatch-test.img.pristine
     (same input every run -> deterministic).
  2. Boots ROS, runs `pacman -S --noconfirm <PKGS>` via luagent.
  3. Writes pacclean.log + post-run image side copies tagged by run id.
  4. Walks the post-run cache, reports # corrupt / chain-truncated files
     and which packages corrupted, so runs are comparable.

Usage:
  scripts/repro_pacman_corruption.py <run_tag> [pkg1 pkg2 ...]

If no packages given, defaults to the smallest set that has been observed to
trigger the bug in <60 minutes.
"""
import json, os, shutil, subprocess, sys, time, urllib.request
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from ros_test import RosQemu

PRISTINE = "/tmp/fatpatch-test.img.pristine"
USB      = "/tmp/fatpatch-test.img"
ISO      = "/home/kreijstal/git/reactos/build_nt62/bootcd.iso"
NAME     = "pacrepro"
MIRROR   = "https://repo.msys2.org/msys/x86_64"
MIRDIR   = "/tmp/pacman_repro/_mirror"
WORK     = "/tmp/pacman_repro"

# observed-to-corrupt set (from cleaninstall.assert run)
OBSERVED_CORRUPT = [
    "gettext", "git", "libgnutls", "libopenssl", "msys2-runtime",
    "perl", "vim", "xz",
]
# default = a 30-package install candidate set covering the corrupt-prone
# packages plus their typical dependencies; experimentally narrowed.
DEFAULT_PKGS = [
    "git", "vim", "perl", "libgnutls", "libopenssl", "msys2-runtime",
    "gettext", "xz",
    # deps frequently pulled along; keep moderate to ensure the bug surfaces
    "openssl", "ca-certificates", "libcurl", "curl", "libssh2",
    "libnghttp2", "libidn2", "libpsl", "libtasn1", "libnettle",
    "libhogweed", "libgcrypt", "libgpg-error", "p11-kit", "libp11-kit",
    "libzstd", "zstd", "zlib", "bzip2",
]

def restore_pristine():
    print(f"[repro] restore {USB} from pristine", flush=True)
    if os.path.exists(USB): os.unlink(USB)
    # cp --reflink for speed on btrfs/xfs; fall back to plain cp
    r = subprocess.run(["cp", "--reflink=auto", PRISTINE, USB])
    if r.returncode != 0:
        shutil.copyfile(PRISTINE, USB)

def run_pacman(c, label, argv, idle_s=900, deadline_s=10800):
    last_recv = [time.time()]
    last_payload = [b""]
    def stamp(prefix, payload):
        last_recv[0] = time.time()
        last_payload[0] = payload
        sys.stdout.write(f"[{time.strftime('%H:%M:%S')}] {prefix}|{payload.decode(errors='replace')}")
        sys.stdout.flush()
    print(f"\n[repro] === {label}: {' '.join(argv)} ===", flush=True)
    c.spawn("C:\\usr\\bin\\pacman.exe", *argv,
            timeout_ms=deadline_s * 1000, idle_timeout_ms=900000)
    t0 = time.time(); deadline = t0 + deadline_s
    while True:
        now = time.time()
        if now > deadline:
            print(f"[repro] {label}: hard deadline {deadline_s}s", flush=True)
            return ("deadline", None)
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
            print(f"[repro] {label}: ROS error: {msg}", flush=True)
            return ("ros_error", msg)
        if status == "exit":
            print(f"\n[repro] {label}: pacman exited {info}", flush=True)
            return ("exit", info)
        if status == "eof":
            print(f"\n[repro] {label}: connection EOF", flush=True)
            return ("eof", None)

def verify_cache(image, run_tag):
    runwork = os.path.join(WORK, f"run_{run_tag}")
    os.makedirs(runwork, exist_ok=True)
    os.makedirs(MIRDIR, exist_ok=True)
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
                urllib.request.urlretrieve(f"{MIRROR}/{name}", mirror)
            except Exception as e:
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

    restore_pristine()
    q = RosQemu(NAME, iso=ISO)
    q.start_luagent(USB)
    print("[repro] waiting for luagent listen", flush=True)
    q.wait_luagent_listen(timeout=180)
    c = q.luagent(hello_timeout=15)

    # one-shot -Syu to refresh the DB on the pristine image
    s1, _ = run_pacman(c, "step1 -Syu",
                       ["C:\\usr\\bin\\pacman.exe", "-Syu", "--noconfirm"],
                       idle_s=300, deadline_s=600)
    if s1 != "exit":
        print(f"[repro] step1 didn't exit cleanly: {s1}; continuing")

    # main install (the bug-trigger)
    argv = ["C:\\usr\\bin\\pacman.exe", "-S", "--noconfirm"] + list(pkgs)
    s2, info = run_pacman(c, "step2 install", argv,
                          idle_s=1800, deadline_s=10800)
    print(f"[repro] step2 result = {s2}")

    # drain
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

    # snapshot post-run
    post = f"/tmp/fatpatch-test.img.run_{run_tag}"
    shutil.copyfile(USB, post)
    print(f"[repro] image snapshot saved to {post}")

    rows = verify_cache(post, run_tag)
    n_ok = sum(1 for r in rows if r["status"] == "ok")
    n_diff = sum(1 for r in rows if r["status"] == "diff")
    n_size = sum(1 for r in rows if r["status"] == "size_differs")
    n_other = sum(1 for r in rows if r["status"] not in ("ok","diff","size_differs"))
    summary = {
        "run_tag": run_tag,
        "step1_status": s1,
        "step2_status": s2,
        "ok": n_ok, "byte_diff": n_diff, "size_diff": n_size, "other": n_other,
        "total": len(rows),
        "corrupt_files": [r for r in rows if r["status"] != "ok"],
    }
    out = os.path.join(WORK, f"repro_{run_tag}.json")
    os.makedirs(WORK, exist_ok=True)
    with open(out, "w") as f:
        json.dump(summary, f, indent=2)
    print(f"\n[repro] {run_tag}: ok={n_ok} byte_diff={n_diff} size_diff={n_size} other={n_other} total={len(rows)}")
    print(f"[repro] summary -> {out}")
    sys.exit(0 if (n_diff + n_size + n_other) == 0 else 2)

if __name__ == "__main__":
    main()
