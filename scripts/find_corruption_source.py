#!/usr/bin/env python3
"""For each corrupted file, search for byte runs from its corrupt tail
   inside every OTHER file in the cache (and in mirror copies) to determine
   if the stale data came from a prior file read."""
import os, sys, subprocess

WORK   = "/tmp/pacman_repro"
RUN    = sys.argv[1] if len(sys.argv) > 1 else "run5"
RUNDIR = os.path.join(WORK, f"run_{RUN}")
MIRDIR = os.path.join(WORK, "_mirror")

# corrupt files: bash, gawk, ncurses (from run5 verify)
CORRUPT = {
    "bash-5.3.009-1-x86_64.pkg.tar.zst":   0x129000,
    "gawk-5.4.0-1-x86_64.pkg.tar.zst":     0x9a000,
    "ncurses-6.6-1-x86_64.pkg.tar.zst":    0x76000,
}

def search_run(corrupt_path, first_diff, candidates):
    """Take 1024-byte fingerprints from the corrupt tail and search each
       candidate file for them.  Returns list of (fp_offset, found_in_file,
       found_at_offset)."""
    data = open(corrupt_path, "rb").read()
    tail_start = first_diff
    tail_end = len(data)
    fps = []
    for off in range(tail_start, tail_end, 65536):
        fps.append((off, data[off:off+128]))
    hits = []
    for cand in candidates:
        cdata = open(cand, "rb").read()
        for fp_off, fp in fps:
            idx = cdata.find(fp)
            if idx >= 0:
                hits.append((fp_off, cand, idx, len(fp)))
    return hits

def main():
    candidates = []
    for d in [RUNDIR, MIRDIR]:
        if not os.path.isdir(d): continue
        for n in os.listdir(d):
            if not n.endswith(".pkg.tar.zst"): continue
            if n in CORRUPT: continue   # don't search corrupt-self
            candidates.append(os.path.join(d, n))
    print(f"searching {len(candidates)} candidate files for stale-data hits")
    for name, first_diff in CORRUPT.items():
        path = os.path.join(RUNDIR, name)
        if not os.path.exists(path):
            print(f"SKIP {name}: not extracted")
            continue
        print(f"\n== {name} (corrupt from {first_diff:#x}) ==")
        hits = search_run(path, first_diff, candidates)
        if not hits:
            print(f"  NO HITS — corrupt bytes don't appear in any other cached/mirror file")
        else:
            for fp_off, cand, idx, ln in hits:
                print(f"  fp@{fp_off:#x} of len {ln} found in "
                      f"{os.path.basename(cand)} @ {idx:#x}")

if __name__ == "__main__":
    main()
