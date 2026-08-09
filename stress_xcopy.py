#!/usr/bin/env python3
"""Paced bug5 stress: xcopy the whole msys64 tree from the host SMB share to
local NTFS via the fixed smb2rdr (now that wildcards resolve). ONE process,
self-paced (xcopy emits per-file stdout so the idle-timeout won't fire and
copies never stack). Watch COM1 for the MM page-reuse family."""
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
    # The designed workload: recursive tree pull -> smb2rdr read + dir-enum +
    # NTFS create/write churn + Cc + MM. Self-paced via per-file output.
    run(lu, "xcopy-msys64",
        r"xcopy /E /I /Y /C \\10.0.2.2\public\msys64 C:\msys64",
        win=5400, idle=300)
    run(lu, "count", r"dir /s /a-d C:\msys64 | find  /c File", win=120, idle=60)
    print("=== xcopy stress done ===", flush=True)
    time.sleep(2)
    q.kill()

if __name__ == '__main__':
    main()
