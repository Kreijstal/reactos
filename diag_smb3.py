#!/usr/bin/env python3
"""Narrow the SMB hang: open+query a specific file (dir <file>) vs a tiny read
(type test.txt) vs a small binary copy. Ensure smb2d is RUNNING first."""
import time
import drive_msys_smb as D

CMD = r"C:\ReactOS\system32\cmd.exe"

def run(lu, label, cmdline, win=40):
    print(f"\n--- {label}: {cmdline}", flush=True)
    lu.spawn(CMD, CMD, "/c", cmdline, timeout_ms=win*1000, idle_timeout_ms=win*1000)
    res, kv = lu.stream(deadline=time.time() + win)
    print(f"--- {label} -> {res} {kv}", flush=True)

def main():
    q = D.boot_hd_net()
    lu = D.wait_luagent(q)
    # make sure the daemon is up (auto-start races the shell login)
    run(lu, "ensure-smb2d", "sc start smb2d & ping -n 4 127.0.0.1 >nul & sc query smb2d", win=40)
    run(lu, "query-file",  r"dir \\10.0.2.2\public\msys.png", win=40)          # open+query, no bulk read
    run(lu, "tiny-read",   r"type \\10.0.2.2\public\test.txt", win=40)         # 300-byte read
    run(lu, "small-copy",  r"copy /Y \\10.0.2.2\public\test.txt C:\t.txt & dir C:\t.txt", win=60)
    print("=== diag3 done ===", flush=True)
    time.sleep(2)
    q.kill()

if __name__ == '__main__':
    main()
