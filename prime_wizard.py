#!/usr/bin/env python3
"""Boot the freshly-installed HD; if the GUI first-boot wizard is up, drive it
to completion with Enter via the monitor. The wizard's completion triggers a
reboot which (under -no-reboot) exits qemu. We just wait for that exit."""
import time, os, socket
import drive_msys_smb as D

def mon(cmd):
    try:
        s = socket.socket(socket.AF_UNIX); s.connect(D.SOCK)
        s.sendall((cmd + "\n").encode()); time.sleep(0.3); s.close()
    except Exception as e:
        print("mon err", e, flush=True)

def main():
    q = D.boot_hd_net(gdb=False)
    print(f"booted PID={q.pid}; driving wizard with Enter until reboot-exit", flush=True)
    # The wizard auto-appears after session init. Mash Enter slowly; once the
    # wizard finalizes the guest reboots and qemu (PID) exits.
    deadline = time.time() + 360
    while time.time() < deadline:
        if not D.os.path.exists(f"/proc/{q.pid}"):
            print("qemu exited — wizard finalized + rebooted", flush=True)
            return 0
        mon("sendkey ret")
        time.sleep(4)
    print("timeout: wizard did not finalize", flush=True)
    q.kill()
    return 1

if __name__ == '__main__':
    raise SystemExit(main())
