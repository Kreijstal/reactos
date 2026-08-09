#!/usr/bin/env python3
"""Verify the readdir-filter fix: dir/copy of a SINGLE file must touch only
that file, not glob the whole share. Then a small wildcard sanity check."""
import time
import drive_msys_smb as D

CMD = r"C:\ReactOS\system32\cmd.exe"

def run(lu, label, cmdline, win=60):
    print(f"\n--- {label}: {cmdline}", flush=True)
    lu.spawn(CMD, CMD, "/c", cmdline, timeout_ms=win*1000, idle_timeout_ms=win*1000)
    res, kv = lu.stream(deadline=time.time() + win)
    print(f"--- {label} -> {res} {kv}", flush=True)

def wait_smb2d(lu):
    """smb2d is Start=2 auto but races shell login; force-start it and give it
    a generous settle so the SMB tests don't hit a spurious BAD_NETPATH."""
    run(lu, "start-smb2d", "sc start smb2d", win=30)
    time.sleep(12)
    run(lu, "query-smb2d", "sc query smb2d", win=30)

def main():
    q = D.boot_hd_net()
    lu = D.wait_luagent(q)
    wait_smb2d(lu)
    # THE failing case: dir of one file must list exactly one file
    run(lu, "dir-one",   r"dir \\10.0.2.2\public\test.txt", win=40)
    # copy of one small file to a local file must complete with the right size
    run(lu, "copy-one",  r"copy /Y \\10.0.2.2\public\test.txt C:\v.txt & dir C:\v.txt & type C:\v.txt", win=60)
    # copy to CON must no longer flood (used to drag in the 78MB/938MB binaries)
    run(lu, "copy-con",  r"copy /Y \\10.0.2.2\public\test.txt CON", win=40)
    # wildcard cases that the literal matcher failed (DOS wildcards):
    run(lu, "wild-bat",  r"dir \\10.0.2.2\public\*.bat", win=40)   # -> nlmrun.bat etc.
    run(lu, "wild-star", r"dir \\10.0.2.2\public\*", win=40)       # -> whole dir (legit)
    run(lu, "wild-txt",  r"dir \\10.0.2.2\public\*.txt", win=40)   # -> test.txt, log.txt...
    run(lu, "wild-q",    r"dir \\10.0.2.2\public\test.???", win=40)# -> test.txt
    print("=== verify done ===", flush=True)
    time.sleep(2)
    q.kill()

if __name__ == '__main__':
    main()
