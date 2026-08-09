#!/usr/bin/env python3
"""Boot nt10msys with a private gdb stub (:1244), bring up luagent, kick off
xcopy of the msys64 tree NON-blocking, and poll COM1 for the rxce.c:2383
FCB-tracker assert. On hit, leave the guest halted in KDB (qemu alive) so gdb
can grab the backtrace; print the smb2rdr.sys load base for symbol resolution."""
import os, time, subprocess, re
import drive_msys_smb as D

GDB_PORT = 1244
CMD = r"C:\ReactOS\system32\cmd.exe"

def launch():
    for pid in subprocess.run(["pgrep", "-f", rf"qemu.*-name {D.NAME}\b"],
                              capture_output=True, text=True).stdout.split():
        try: os.kill(int(pid), 9)
        except ProcessLookupError: pass
    time.sleep(0.5)
    for f in (D.LOG, D.SOCK, D.COM2):
        if os.path.exists(f):
            os.unlink(f)
    cmd = [
        "qemu-system-x86_64", "-m", "1024",
        "-drive", f"file={D.DISK},format=raw,if=ide", "-boot", "c",
        "-netdev", f"user,id=net0,hostfwd=tcp:127.0.0.1:{D.HOST_PORT}-:7000",
        "-device", "rtl8139,netdev=net0",
        "-chardev", f"file,id=com1,path={D.LOG}",
        "-chardev", f"socket,id=com2,path={D.COM2},server=on,wait=off",
        "-serial", "chardev:com1", "-serial", "chardev:com2",
        "-monitor", f"unix:{D.SOCK},server,nowait",
        "-rtc", "base=2026-06-02T12:00:00,clock=host",
        "-display", "gtk", "-no-reboot", "-enable-kvm",
        "-name", D.NAME, "-daemonize",
        "-gdb", f"tcp::{GDB_PORT}",
    ]
    subprocess.run(cmd, check=True, capture_output=True)
    time.sleep(0.5)
    out = subprocess.run(["pgrep", "-f", rf"qemu.*-name {D.NAME}\b"],
                         capture_output=True, text=True).stdout.split()
    q = D.RosQemu(name=D.NAME, iso=D.ISO, disk=D.DISK)
    q.pid = int(out[0]) if out else None
    q.com2_sock = D.COM2
    q.host_port = D.HOST_PORT
    print(f"=== boot PID={q.pid} gdb=:{GDB_PORT} ===", flush=True)
    return q

def main():
    q = launch()
    lu = D.wait_luagent(q)
    # start daemon, settle past the login race
    lu.spawn(CMD, CMD, "/c", "sc start smb2d", timeout_ms=30000, idle_timeout_ms=20000)
    lu.stream(deadline=time.time() + 30)
    time.sleep(12)
    # kick off xcopy NON-blocking (spawn, don't stream to completion)
    print("=== launching xcopy (non-blocking) ===", flush=True)
    lu.spawn(CMD, CMD, "/c",
             r"xcopy /E /I /Y /C \\10.0.2.2\public\msys64 C:\msys64",
             timeout_ms=3600000, idle_timeout_ms=3600000)
    # poll COM1 for the assert
    deadline = time.time() + 300
    hit = False
    while time.time() < deadline:
        try:
            with open(D.LOG, "r", errors="replace") as fh:
                txt = fh.read()
        except FileNotFoundError:
            txt = ""
        if "AcquireReleaseFcbTrackerX" in txt:
            hit = True
            break
        time.sleep(3)
    if not hit:
        print("=== NO ASSERT within window ===", flush=True)
        return
    print("=== ASSERT HIT — guest halted in KDB, qemu LEFT ALIVE ===", flush=True)
    m = re.search(r"smb2rdr\.sys at ([0-9A-Fa-f]+)", txt)
    if m:
        print(f"SMB2RDR_BASE=0x{m.group(1)}", flush=True)
    m2 = re.search(r"smb2rdr_xx|smb2rdr", txt)
    print(f"QEMU_PID={q.pid} GDB_PORT={GDB_PORT}", flush=True)

if __name__ == '__main__':
    main()
