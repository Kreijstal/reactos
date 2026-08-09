#!/usr/bin/env python3
"""Boot the installed nt10msys HD networked, bring up luagent, and probe why
\\10.0.2.2\public returns BAD_NETPATH: is smb2d running, did DHCP give a lease,
what does `net use` say, and does a direct retry behave differently."""
import time
import drive_msys_smb as D

CMD = r"C:\ReactOS\system32\cmd.exe"

def run(lu, label, cmdline, win=60):
    print(f"\n--- {label}: {cmdline}", flush=True)
    lu.spawn(CMD, CMD, "/c", cmdline, timeout_ms=win*1000, idle_timeout_ms=win*1000)
    res, kv = lu.stream(deadline=time.time() + win)
    print(f"--- {label} -> {res} {kv}", flush=True)

def main():
    q = D.boot_hd_net()
    lu = D.wait_luagent(q)
    run(lu, "ipconfig",  "ipconfig /all")
    run(lu, "sc-smb2d",  "sc query smb2d")
    run(lu, "tasklist",  "tasklist")
    run(lu, "net-use",   r"net use \\10.0.2.2\public")
    run(lu, "dir-retry", r"dir \\10.0.2.2\public")
    run(lu, "ping-host", "ping -n 2 10.0.2.2")
    print("=== diag done ===", flush=True)
    time.sleep(2)
    q.kill()

if __name__ == '__main__':
    main()
