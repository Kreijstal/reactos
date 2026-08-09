#!/usr/bin/env python3
"""Post-run verifier: extract every .zst from the FAT image cache, compare
   against the upstream mirror, log first-diff offset and total corrupt size
   for any file that differs.  Outputs a table per run plus a JSON summary
   at /tmp/pacman_repro/run_<n>.json so we can compare runs."""
import json, os, subprocess, sys, urllib.request, hashlib, time

IMG     = "/tmp/fatpatch-test.img"
WORK    = "/tmp/pacman_repro"
MIRROR  = "https://repo.msys2.org/msys/x86_64"
MIRDIR  = "/tmp/pacman_repro/_mirror"

def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)

def list_cache():
    r = run(["mdir", "-i", f"{IMG}@@1M", "-b", "::/var/cache/pacman/pkg/"])
    out = []
    for line in r.stdout.splitlines():
        line = line.strip()
        if not line: continue
        # mdir -b prints absolute path inside the image; strip ::/.../
        name = os.path.basename(line)
        if name.endswith(".pkg.tar.zst"):
            out.append(name)
    return sorted(out)

def extract(name, dest):
    r = run(["mcopy", "-i", f"{IMG}@@1M", "-o",
             f"::/var/cache/pacman/pkg/{name}", dest])
    if r.returncode != 0:
        raise RuntimeError(f"mcopy {name} failed: {r.stderr}")

def fetch_mirror(name):
    os.makedirs(MIRDIR, exist_ok=True)
    p = os.path.join(MIRDIR, name)
    if os.path.exists(p) and os.path.getsize(p) > 0:
        return p
    url = f"{MIRROR}/{name}"
    sys.stdout.write(f"  fetch {name}... "); sys.stdout.flush()
    urllib.request.urlretrieve(url, p)
    sys.stdout.write(f"{os.path.getsize(p)} bytes\n")
    return p

def first_diff(a_path, b_path):
    a = open(a_path, "rb").read()
    b = open(b_path, "rb").read()
    if len(a) != len(b):
        return {"status": "size_differs", "size": len(a),
                "mirror_size": len(b)}
    if a == b:
        return {"status": "ok", "size": len(a)}
    j = 0
    while j < len(a) and a[j] == b[j]:
        j += 1
    last = len(a) - 1
    while last > j and a[last] == b[last]:
        last -= 1
    corrupt = last - j + 1
    # fingerprint the corrupt tail in 4 KiB chunks for stale-page analysis
    chunk_hashes = []
    for off in range(j & ~0xfff, len(a), 4096):
        h = hashlib.md5(a[off:off+4096]).hexdigest()[:12]
        chunk_hashes.append((off, h))
    return {"status": "diff", "size": len(a),
            "first_diff": j, "off_mod_4096": j % 4096,
            "last_diff": last, "corrupt_bytes": corrupt,
            "tail_md5": hashlib.md5(a[j:last+1]).hexdigest(),
            "chunk_hashes": chunk_hashes,
            "tail_first_32": a[j:j+32].hex(),
            "mirror_first_32_at_j": b[j:j+32].hex()}

def main():
    runtag = sys.argv[1] if len(sys.argv) > 1 else f"r{int(time.time())}"
    runwork = os.path.join(WORK, f"run_{runtag}")
    os.makedirs(runwork, exist_ok=True)
    cache = list_cache()
    print(f"[verify] cache has {len(cache)} packages")
    rows = []
    for name in cache:
        local = os.path.join(runwork, name)
        try:
            extract(name, local)
        except Exception as e:
            print(f"  SKIP {name}: {e}")
            continue
        try:
            mirror = fetch_mirror(name)
        except Exception as e:
            print(f"  SKIP mirror {name}: {e}")
            continue
        info = first_diff(local, mirror)
        info["name"] = name
        rows.append(info)
        s = info["status"]
        if s == "diff":
            unique_chunks = len(set(h for _, h in info["chunk_hashes"]))
            total_chunks = len(info["chunk_hashes"])
            print(f"  CORRUPT {name}: size={info['size']} "
                  f"diff@{info['first_diff']:#x} "
                  f"(/4096={info['first_diff']//4096}, "
                  f"mod={info['off_mod_4096']}) "
                  f"corrupt={info['corrupt_bytes']} "
                  f"unique_4k_chunks={unique_chunks}/{total_chunks}")
        elif s == "size_differs":
            print(f"  SIZE_MISMATCH {name}: local={info['size']} "
                  f"mirror={info['mirror_size']}")
        else:
            print(f"  ok {name}")
    summary = os.path.join(WORK, f"run_{runtag}.json")
    with open(summary, "w") as f:
        json.dump(rows, f, indent=2)
    print(f"[verify] summary written to {summary}")
    bad = [r for r in rows if r["status"] != "ok"]
    print(f"[verify] {len(bad)}/{len(rows)} files corrupt")
    for r in bad:
        if r["status"] == "diff":
            print(f"   - {r['name']}  diff@{r['first_diff']:#x}  "
                  f"corrupt={r['corrupt_bytes']}  "
                  f"tail_md5={r['tail_md5']}")
        else:
            print(f"   - {r['name']}  {r['status']}  "
                  f"local={r['size']} mirror={r.get('mirror_size','?')}")

if __name__ == "__main__":
    main()
