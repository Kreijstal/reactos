#!/usr/bin/env python3
"""Boot networked, bring up luagent, then manually start smb2d and retry the
share to validate the downstream SMB2 stack and isolate the auto-start bug."""
import time
import drive_msys_smb as D

CMD = r"C:\ReactOS\system32\cmd.exe"

def run(lu, label, cmdline, win=120):
    print(f"\n--- {label}: {cmdline}", flush=True)
    lu.spawn(CMD, CMD, "/c", cmdline, timeout_ms=win*1000, idle_timeout_ms=win*1000)
    res, kv = lu.stream(deadline=time.time() + win)
    print(f"--- {label} -> {res} {kv}", flush=True)

def main():
    q = D.boot_hd_net()
    lu = D.wait_luagent(q)
    run(lu, "exists",     r"dir C:\ReactOS\system32\smb2d.exe")
    run(lu, "sc-start",   "sc start smb2d")
    time.sleep(3)
    run(lu, "sc-query",   "sc query smb2d")
    run(lu, "dir-share",  r"dir \\10.0.2.2\public")
    run(lu, "copy-one",   r"copy /Y \\10.0.2.2\public\msys.png C:\msys.png & dir C:\msys.png")
    print("=== diag2 done ===", flush=True)
    time.sleep(2)
    q.kill()

if __name__ == '__main__':
    main()
