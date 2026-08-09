#!/usr/bin/env python3
"""Separate the copy hang: NTFS-write side vs copy's source-handle metadata
queries. type>file = SMB read + NTFS write (no copy metadata). copy>CON =
copy source handling, no NTFS dest write. copy>file = the failing case."""
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
    run(lu, "ensure", "sc start smb2d & ping -n 4 127.0.0.1 >nul", win=30)
    # baseline: pure local NTFS write works?
    run(lu, "local-write", r"echo hello-local> C:\loc.txt & type C:\loc.txt", win=30)
    # SMB read + NTFS write via redirection (no copy metadata queries)
    run(lu, "type-redir",  r"type \\10.0.2.2\public\test.txt > C:\out1.txt & dir C:\out1.txt", win=40)
    # copy source handling but dest = console (no NTFS dest write)
    run(lu, "copy-con",    r"copy /Y \\10.0.2.2\public\test.txt CON", win=40)
    # the failing case
    run(lu, "copy-file",   r"copy /Y \\10.0.2.2\public\test.txt C:\out2.txt", win=40)
    print("=== diag4 done ===", flush=True)
    time.sleep(2)
    q.kill()

if __name__ == '__main__':
    main()
