#!/usr/bin/env python3
"""Over-the-top unattended NTFS reinstall onto reactos.qcow2 to recover the
SYSTEM hive corrupted by earlier SIGKILLs, while preserving the MSYS2 install.

bootcdntfs.iso: FormatPartition=0, AutoPartition=0, FsType=2 (NTFS) ->
refreshes OS + registry (fresh SYSTEM hive) in place, keeping existing files.
Carries the fixed SMP-swap-busy kernel (ntkrnlmp md5 9cfaaf0f).

Boot menu DefaultOS=Setup_Debug auto-boots in 2s -> serial debug on COM1.
QEMU -no-reboot: process exits on each guest reboot; we relaunch from HD.
"""
import sys, time, os, subprocess
sys.path.insert(0, '/home/kreijstal/git/reactos')
from ros_test import RosQemu

DISK = '/home/kreijstal/git/reactos/reactos.qcow2'
ISO  = '/home/kreijstal/git/reactos/build_nt10/bootcdntfs.iso'
os.environ.setdefault('DISPLAY', ':10.0')

q = RosQemu(name='rosrecover', iso=ISO, disk=DISK)

def launch(boot, cd):
    q._mon = None                          # drop stale monitor socket
    for f in (q.log_path, q.sock_path):
        if os.path.exists(f):
            os.unlink(f)
    base = ['qemu-system-x86_64', '-m', '2048',
            '-serial', f'file:{q.log_path}',
            '-monitor', f'unix:{q.sock_path},server,nowait',
            '-display', 'gtk',
            '-netdev', 'user,id=n0', '-device', 'rtl8139,netdev=n0',
            '-no-reboot', '-enable-kvm', '-s', '-daemonize']
    if cd:
        # Stage 1: the bootable HD wins over -boot d / CD bootindex, so wire the
        # CD as PRIMARY master (ide.0) and the HD as SECONDARY master (ide.1) and
        # force CD boot.  usetup then runs the unattended over-the-top install,
        # overwriting the corrupt SYSTEM hive on the HD with a fresh one.
        cmd = base + [
            '-drive', f'id=cd0,file={ISO},format=raw,if=none,media=cdrom',
            '-device', 'ide-cd,drive=cd0,bus=ide.0,unit=0,bootindex=0',
            '-drive', f'id=hd0,file={DISK},format=qcow2,if=none',
            '-device', 'ide-hd,drive=hd0,bus=ide.1,unit=0,bootindex=1',
            '-boot', 'd']
    else:
        # Stage 2: HD only -> boots the freshly-installed system.
        cmd = base + [
            '-drive', f'file={DISK},format=qcow2,if=ide',
            '-boot', 'c']
    subprocess.run(cmd, check=True, capture_output=True)
    time.sleep(0.5)
    from ros_test import _qemu_pids_for_disk
    pids = _qemu_pids_for_disk(DISK)
    q.pid = pids[0] if pids else None
    print(f"  launched boot={boot} cd={cd} pid={q.pid}", flush=True)

def tail(n=12):
    for l in q.log_lines()[-n:]:
        print("   |", l.rstrip(), flush=True)

# ---- Stage 1: text-mode unattended install from CD --------------------------
print("=== STAGE 1: unattended over-the-top NTFS install (boot CD) ===", flush=True)
launch('d', cd=True)

start = time.time()
install_done = False
while time.time() - start < 1800:          # up to 30 min for file copy
    if not q.is_alive():
        print(f"  QEMU exited (guest reboot) after {time.time()-start:.0f}s", flush=True)
        install_done = True
        break
    if q.crashed():
        print(f"  CRASH during install at {time.time()-start:.0f}s:", flush=True)
        tail(20)
        sys.exit(2)
    lines = q.log_lines()
    if any('NtfsShutdown()' in l or 'Install NTFS bootcode:' in l
           or 'SETUP_PAGE:SUCCESS' in l for l in lines[-200:]):
        # finalization reached; nudge ENTER toward reboot
        q.send('ret')
    time.sleep(3)

if not install_done:
    print("  STAGE 1 timed out (no reboot). Last serial:", flush=True)
    tail(30)
    sys.exit(3)

# ---- Stage 2: boot from HD across each reboot until desktop -----------------
print("=== STAGE 2: boot installed system from HD ===", flush=True)
for attempt in range(1, 7):
    print(f"--- HD boot attempt {attempt} ---", flush=True)
    launch('c', cd=False)
    bstart = time.time()
    reached = None
    while time.time() - bstart < 240:
        if q.crashed():
            print(f"  CRASH on HD boot {attempt}:", flush=True); tail(20)
            reached = 'crash'; break
        if not q.is_alive():
            print(f"  qemu exited (reboot) after {time.time()-bstart:.0f}s", flush=True)
            reached = 'reboot'; break
        lines = q.log_lines()
        if any('Explorer' in l or 'AUTORUN' in l or 'GuiRunOnce' in l
               or 'Session 0 is ready' in l for l in lines[-100:]):
            print(f"  session/desktop marker at {time.time()-bstart:.0f}s", flush=True)
            reached = 'desktop'; break
        time.sleep(2)
    if reached == 'crash':
        sys.exit(4)
    if reached == 'desktop':
        # let it settle, screenshot to confirm desktop
        time.sleep(40)
        p, _ = q.screenshot('recovered_desktop')
        print(f"  screenshot -> {p}", flush=True)
        tail(8)
        print("=== RECOVER_DONE: installed system reached desktop ===", flush=True)
        # clean ACPI shutdown to persist the fresh hive
        q.monitor_cmd('system_powerdown')
        # wait for clean exit (NtfsShutdown flush) — do NOT SIGKILL
        for _ in range(60):
            if not q.is_alive():
                print("  clean powerdown complete", flush=True); break
            time.sleep(2)
        sys.exit(0)
    if reached is None:
        print(f"  HD boot {attempt} stalled (no marker in 240s):", flush=True); tail(20)
        # try again — second-stage may need another reboot cycle
print("=== exhausted HD boot attempts ===", flush=True)
sys.exit(5)
