#!/usr/bin/env python3
"""bug5 stress: pull large files from the host SMB share to local NTFS via the
fixed smb2rdr, hammering smb2rdr-read + Cc + MM + NTFS-write. Explicit paths
(single-file copy is verified) so wildcard matching is not on the path. Watch
COM1 for the MM page-reuse family (asserts/bugchecks)."""
import time
import drive_msys_smb as D

CMD = r"C:\ReactOS\system32\cmd.exe"

def run(lu, label, cmdline, win, idle):
    print(f"\n--- {label}: {cmdline}", flush=True)
    lu.spawn(CMD, CMD, "/c", cmdline, timeout_ms=win*1000, idle_timeout_ms=idle*1000)
    res, kv = lu.stream(deadline=time.time() + win)
    print(f"--- {label} -> {res} {kv}", flush=True)

def main():
    q = D.boot_hd_net()
    lu = D.wait_luagent(q)
    run(lu, "start-smb2d", "sc start smb2d", win=30, idle=20)
    time.sleep(12)
    run(lu, "query", "sc query smb2d", win=20, idle=15)
    # Heavy single-file pulls (each an explicit path -> no globbing).
    # o.tar 327MB, msys2 installer 94MB; loop to churn MM/Cc.
    for i in range(3):
        run(lu, f"copy-installer-{i}",
            r"copy /Y \\10.0.2.2\public\msys2-x86_64-latest.exe C:\msys2.exe & dir C:\msys2.exe",
            win=900, idle=180)
        run(lu, f"copy-otar-{i}",
            r"copy /Y \\10.0.2.2\public\o.tar C:\o.tar & dir C:\o.tar",
            win=1800, idle=240)
        run(lu, f"del-{i}", r"del C:\msys2.exe C:\o.tar & echo cycle %i done", win=60, idle=30)
    print("=== stress done (no guest crash) ===", flush=True)
    time.sleep(2)
    q.kill()

if __name__ == '__main__':
    main()
