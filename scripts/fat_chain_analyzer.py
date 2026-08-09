#!/usr/bin/env python3
"""Diagnose corruption locus on a FAT32 image.

For every file under /var/cache/pacman/pkg/, walks the on-disk FAT chain,
records (file -> [cluster ids]). Then:

  Phase 1: chain-overlap detection. If two distinct files share a cluster
           index, that is FS-level chain aliasing (allocator bug).

  Phase 2: byte-level corruption locus. For each corrupt file:
           - read the chain
           - for each cluster in chain, compare its 64KB on-disk content
             against the same VBO range from the upstream mirror
           - report first diverging cluster and what the content matches
             on disk (search other clusters for the EXPECTED bytes)

Distinguishes:
  * chain aliasing  -> FS allocator bug
  * cluster bytes wrong but no fingerprint match -> network/USB bytes
  * cluster bytes wrong + fingerprint of EXPECTED bytes elsewhere -> cache
    aliasing (page contents written to wrong cluster)
"""
import os, struct, sys, hashlib, urllib.request

IMG = sys.argv[1] if len(sys.argv) > 1 else "/tmp/fatpatch-test.img.cleaninstall.assert"
PART_OFFSET = 1 * 1024 * 1024  # MBR partition starts at 1 MiB
MIRROR_DIR = "/tmp/pacman_repro/_mirror"

with open(IMG, "rb") as f:
    f.seek(PART_OFFSET)
    bpb = f.read(512)

# FAT32 BPB layout
BytesPerSector  = struct.unpack_from("<H", bpb, 11)[0]
SecPerClus      = bpb[13]
ResvdSecCnt     = struct.unpack_from("<H", bpb, 14)[0]
NumFATs         = bpb[16]
FATSz32         = struct.unpack_from("<I", bpb, 36)[0]
RootClus        = struct.unpack_from("<I", bpb, 44)[0]
TotSec32        = struct.unpack_from("<I", bpb, 32)[0]

ClusterSize     = BytesPerSector * SecPerClus
FAT0_OffSec     = ResvdSecCnt
FAT0_Off        = PART_OFFSET + FAT0_OffSec * BytesPerSector
DataStartSec    = ResvdSecCnt + NumFATs * FATSz32
DataStart       = PART_OFFSET + DataStartSec * BytesPerSector
TotalClusters   = (TotSec32 - DataStartSec) // SecPerClus + 2  # 0,1 reserved

print(f"BPB: BytesPerSector={BytesPerSector} SecPerClus={SecPerClus} "
      f"ClusterSize={ClusterSize} ResvdSec={ResvdSecCnt} NumFATs={NumFATs} "
      f"FATSz32={FATSz32} RootClus={RootClus} TotSec32={TotSec32} "
      f"TotalClusters={TotalClusters}")
print(f"FAT0 byte offset: {FAT0_Off:#x}, data byte offset: {DataStart:#x}")

# load FAT0 into memory
with open(IMG, "rb") as f:
    f.seek(FAT0_Off)
    FAT0 = f.read(FATSz32 * BytesPerSector)

EOC_MIN = 0x0FFFFFF8

def fat_get(idx):
    if idx >= len(FAT0) // 4:
        return None
    return struct.unpack_from("<I", FAT0, idx * 4)[0] & 0x0FFFFFFF

def chain_walk(start, max_clusters=200000):
    out = []
    seen = set()
    cur = start
    while cur >= 2 and cur < EOC_MIN:
        if cur in seen:
            out.append(("LOOP", cur))
            break
        seen.add(cur)
        out.append(cur)
        nxt = fat_get(cur)
        if nxt is None:
            out.append(("OOR_NEXT", nxt))
            break
        cur = nxt
        if len(out) > max_clusters:
            out.append(("TRUNC_AT_LIMIT", None))
            break
    return out

def read_cluster(idx):
    off = DataStart + (idx - 2) * ClusterSize
    with open(IMG, "rb") as f:
        f.seek(off)
        return f.read(ClusterSize)

# parse short dirents under given dir-cluster
def parse_dir(dir_cluster, depth=0, prefix="", visited=None):
    if visited is None: visited = set()
    if dir_cluster in visited: return []
    visited.add(dir_cluster)

    chain = chain_walk(dir_cluster)
    chain = [c for c in chain if isinstance(c, int)]
    raw = b"".join(read_cluster(c) for c in chain)

    out = []
    lfn_acc = []
    for off in range(0, len(raw), 32):
        e = raw[off:off+32]
        if len(e) < 32: break
        if e[0] == 0x00: break
        if e[0] == 0xE5:
            lfn_acc = []
            continue
        attr = e[11]
        if attr == 0x0F:
            # LFN — skip; we use 8.3 name for simplicity
            continue
        # short entry
        name = e[0:11].decode("ascii", "replace").rstrip()
        first_lo = struct.unpack_from("<H", e, 26)[0]
        first_hi = struct.unpack_from("<H", e, 20)[0]
        first    = (first_hi << 16) | first_lo
        size     = struct.unpack_from("<I", e, 28)[0]
        lfn_acc = []
        if name in (".", ".."): continue
        out.append({"name": name, "first": first, "size": size, "attr": attr,
                    "path": prefix + "/" + name})
        if attr & 0x10:  # subdir
            out.extend(parse_dir(first, depth+1, prefix + "/" + name, visited))
    return out

# walk root then drill down to /var/cache/pacman/pkg/
all_entries = parse_dir(RootClus)
pkg_dir = [e for e in all_entries
           if e["path"].lower().startswith("/var/cache/pacman/pkg/")
           and not (e["attr"] & 0x10)]
print(f"\n/var/cache/pacman/pkg/ has {len(pkg_dir)} files (parsed via 8.3 names)")

# Phase 1: build cluster -> [files] index
cluster_owners = {}
file_chains = {}
for e in pkg_dir:
    if e["first"] == 0: continue
    chain = chain_walk(e["first"])
    chain_clusters = [c for c in chain if isinstance(c, int)]
    file_chains[e["name"]] = (chain_clusters, chain, e["size"])
    for c in chain_clusters:
        cluster_owners.setdefault(c, []).append(e["name"])

shared = {c: owners for c, owners in cluster_owners.items() if len(owners) > 1}
print(f"\nPhase 1: chain-overlap detection")
if shared:
    print(f"  *** {len(shared)} clusters claimed by multiple files ***")
    for c, owners in list(shared.items())[:20]:
        print(f"    cluster {c}: {owners}")
else:
    print(f"  No cluster shared between dirent chains — chains are disjoint")

# Phase 2: byte-level corruption locus
print(f"\nPhase 2: byte-level locus for corrupt files")

# corrupt set with first_diff (from earlier scan)
CORRUPT_DIFFS = {
    "GETTEX~1.ZST":  0x68000,
    "GIT-2~1.ZST":   0x204000,
    "LIBOPE~1.ZST":  0xfc000,
    "MSYS2-~2.ZST":  0xa5000,  # msys2-runtime
    "PERL-5~1.ZST":  0x2dd000,
    "VIM-9~1.ZST":   0x53c000,
}
# also 'short' ones with chain truncated
TRUNC = {"LIBGNU~1.ZST", "XZ-5~1.ZST"}

def get_mirror_bytes(name8_3):
    # map 8.3 to actual full pkg name by prefix match
    pat = name8_3.split("~")[0].lower()
    for fn in os.listdir(MIRROR_DIR):
        if fn.lower().startswith(pat):
            return open(os.path.join(MIRROR_DIR, fn), "rb").read(), fn
    return None, None

for name8, first_diff in CORRUPT_DIFFS.items():
    if name8 not in file_chains:
        # find via prefix match
        for k in file_chains:
            if k.upper().startswith(name8.split("~")[0]):
                name8 = k; break
    info = file_chains.get(name8)
    if not info:
        print(f"  {name8}: no chain info")
        continue
    chain, raw_chain, size = info
    mirror, mirror_name = get_mirror_bytes(name8)
    if not mirror:
        print(f"  {name8}: no mirror copy")
        continue
    # which cluster covers first_diff
    cluster_idx = first_diff // ClusterSize
    if cluster_idx >= len(chain):
        print(f"  {name8}: first_diff cluster_idx={cluster_idx} but chain only {len(chain)} clusters")
        continue
    bad_cluster = chain[cluster_idx]
    on_disk = read_cluster(bad_cluster)
    expected = mirror[cluster_idx*ClusterSize:(cluster_idx+1)*ClusterSize]
    if on_disk == expected:
        print(f"  {name8}: cluster {bad_cluster} matches mirror?? first_diff inside cluster — checking mid-cluster:")
    diff_off_in_cluster = first_diff % ClusterSize
    # Compare 4KB block at first diff
    block_on_disk = on_disk[diff_off_in_cluster:diff_off_in_cluster+4096]
    block_expected = expected[diff_off_in_cluster:diff_off_in_cluster+4096]
    print(f"\n  === {name8} (mirror {mirror_name}) ===")
    print(f"    chain length = {len(chain)} clusters, total size = {size}")
    print(f"    first_diff = {first_diff:#x} -> cluster index {cluster_idx} (physical cluster {bad_cluster})")
    print(f"    cluster offset in file = {cluster_idx*ClusterSize:#x}")
    print(f"    on-disk first 16 at diff: {block_on_disk[:16].hex()}")
    print(f"    expected   first 16 at diff: {block_expected[:16].hex()}")
    if block_on_disk == block_expected:
        print(f"    block matches mirror at first_diff — diff must be later in file")
    else:
        # Search all data clusters for the expected block (cache-aliased to wrong cluster?)
        h_expected = hashlib.md5(block_expected).digest()
        # also search for the on-disk wrong block elsewhere — maybe it's another file's data
        h_wrong = hashlib.md5(block_on_disk).digest()
        print(f"    expected md5 = {h_expected.hex()[:16]}, on-disk md5 = {h_wrong.hex()[:16]}")
        # cross-reference on-disk wrong block against other pkg files' clusters
        match_owner = None
        for other_name, (other_chain, _, other_size) in file_chains.items():
            if other_name == name8: continue
            for c in other_chain:
                if c == bad_cluster: continue
                cd = read_cluster(c)
                # check if our wrong block appears at any 4K-aligned offset
                for o in range(0, ClusterSize, 4096):
                    if cd[o:o+4096] == block_on_disk:
                        match_owner = (other_name, c, o)
                        break
                if match_owner: break
            if match_owner: break
        if match_owner:
            print(f"    *** wrong block found ALSO in {match_owner[0]} at cluster "
                  f"{match_owner[1]} offset {match_owner[2]:#x} ***")
            print(f"    => cluster aliasing OR cache-page bleeding into wrong cluster")
        else:
            print(f"    wrong block not found elsewhere in cache — bytes are unique")

print(f"\nDone")
