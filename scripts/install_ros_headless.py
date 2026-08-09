#!/usr/bin/env python3
"""Headless NTFS reinstall onto reactos.qcow2.

Same flow as install_reactos_qcow2.py, but:
  * -display none  (the gtk path needs a DISPLAY and silently strands the run
    when launched from a background task)
  * serial log / monitor socket under a caller-supplied directory instead of
    /tmp, because each sandboxed task gets its own /tmp overlay - the driving
    script could see its own rosinstall.log while an outside observer could
    not, which made a hung install indistinguishable from a progressing one.
"""
import os, sys, time
sys.path.insert(0, "/home/kreijstal/git/reactos")
import ros_test
from ros_test import RosQemu, install_ntfs, monitor_copy, drive_first_boot_wizard

OUT  = sys.argv[1] if len(sys.argv) > 1 else "/home/kreijstal/git/reactos/.install_out"
ISO  = "/home/kreijstal/git/reactos/build_nt10/bootcd.iso"
DISK = "/home/kreijstal/git/reactos/reactos.qcow2"
os.makedirs(OUT, exist_ok=True)

_orig_start = RosQemu.start


def start(self, boot="d"):
    import subprocess
    for pid in ros_test._qemu_pids_for_disk(self.disk):
        try:
            os.kill(int(pid), 9)
        except (ProcessLookupError, PermissionError):
            pass
    time.sleep(1)
    for f in [self.log_path, self.sock_path]:
        if os.path.exists(f):
            os.unlink(f)
    # Serial as a SOCKET with logfile= (not file:) so the log still lands on
    # disk for wait()/log_lines() AND we retain a write channel.  base/setup
    # asserts (e.g. fsutil.c:1286 Volume->FormatState == Formatted, which fires
    # transiently right after CreatePartition) drop the guest to a kdb prompt;
    # with a file backend there is no way to answer it and the install wedges
    # forever, indistinguishable from a slow format.
    self.ser_path = self.log_path + ".sock"
    if os.path.exists(self.ser_path):
        os.unlink(self.ser_path)
    cmd = [
        "qemu-system-x86_64", "-m", "2048",
        "-drive", f"file={self.disk},format={self.fmt},if=ide",
        "-boot", boot,
        "-chardev", f"socket,id=ser1,path={self.ser_path},server=on,wait=off,logfile={self.log_path}",
        "-serial", "chardev:ser1",
        "-monitor", f"unix:{self.sock_path},server,nowait",
        "-display", "none",
        "-no-reboot", "-nic", "none", "-enable-kvm", "-daemonize",
    ]
    if boot == "d":
        cmd += ["-cdrom", self.iso]
    subprocess.run(cmd, check=True, capture_output=True)
    time.sleep(0.5)
    pids = ros_test._qemu_pids_for_disk(self.disk)
    self.pid = pids[0] if pids else None
    print(f"QEMU started: PID={self.pid} disk={self.disk} boot={boot}", flush=True)
    return self


RosQemu.start = start


def kdb_watchdog(qq, stop):
    """Answer kdb's 'Break repeatedly, break Once, Ignore, terminate...' prompt
    with Ignore so a transient setup ASSERT does not wedge the whole install.
    Every answered assert is printed, so nothing is silently swallowed."""
    import socket as _s
    seen = 0
    while not stop.is_set():
        try:
            n = 0
            with open(qq.log_path, "rb") as fh:
                data = fh.read()
            n = data.count(b"(boipt)?")
            if n > seen:
                ctx = data.rsplit(b"Assertion failed:", 1)
                what = ctx[1].split(b"\n")[0].strip().decode(errors="replace") if len(ctx) > 1 else "?"
                print(f"KDB_ASSERT_IGNORED #{n}: {what}", flush=True)
                sk = _s.socket(_s.AF_UNIX, _s.SOCK_STREAM)
                sk.connect(qq.ser_path)
                sk.sendall(b"i\n")
                sk.close()
                seen = n
        except Exception:
            pass
        stop.wait(2)



# base/setup trips ASSERT(Volume->FormatState == Formatted) (fsutil.c:1286)
# transiently right after CreatePartition, before any format has run.  It is
# unrelated to the filesystem driver and the watchdog answers it with Ignore,
# but RosQemu.crashed() would still abort every wait() the moment the text
# appears.  Whitelist EXACTLY that assert - anything else (notably an NTFS
# assert from the driver under test) must still fail the run loudly.
_BENIGN = "/base/setup/lib/fsutil.c, line 1286"
_orig_crashed = RosQemu.crashed


def crashed(self):
    tag = _orig_crashed(self)
    if tag is None:
        return None
    lines = self.log_lines()
    asserts = [l for l in lines if "Assertion failed" in l or "Source File" in l]
    if asserts and all((_BENIGN in l) or ("FormatState == Formatted" in l) for l in asserts):
        # "kdb" is the prompt the benign assert itself raises, so it is part of
        # the same event; only genuinely different crash classes should abort.
        other = [t for t, m in RosQemu._CRASH_MARKERS
                 if t not in ("assertion", "kdb") and any(m in l for l in lines)]
        if not other:
            return None
    return tag


RosQemu.crashed = crashed

q = RosQemu(name="rosinstall", iso=ISO, disk=DISK)
q.sock_path = os.path.join(OUT, "rosinstall.sock")
q.log_path = os.path.join(OUT, "rosinstall.log")
print(f"LOG  : {q.log_path}", flush=True)
q.start(boot="d")

import threading
_stop = threading.Event()
threading.Thread(target=kdb_watchdog, args=(q, _stop), daemon=True).start()

try:
    install_ntfs(q)
    print("INSTALL_NTFS_DONE", flush=True)
    if not monitor_copy(q, timeout_per_pct=240):
        print("MONITOR_COPY: failed/stalled", flush=True)
        sys.exit(2)
    print("=== waiting SETUP_PAGE:SUCCESS ===", flush=True)
    q.wait("SETUP_PAGE:SUCCESS", 180)
    q.send("ret")
    deadline = time.time() + 120
    while q.is_alive() and time.time() < deadline:
        time.sleep(1)
    if q.is_alive():
        q.kill()
    print("=== GUI setup phase ===", flush=True)
    q.start(boot="d")
    drive_first_boot_wizard(q)
    print("INSTALL_COMPLETE", flush=True)
    _stop.set()
finally:
    try:
        q.kill()
    except Exception:
        pass
