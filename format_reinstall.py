#!/usr/bin/env python3
"""Clean unattended FORMAT reinstall onto reactos.qcow2 (user-approved wipe).

The HD's MBR 55AA signature is already zeroed, so SeaBIOS skips the (corrupt)
HD and boots the CD installer.  bootcdregtest.iso is fully unattended:
AutoPartition=1 + FormatPartition=1 -> creates a fresh partition, formats,
installs the fixed SMP kernel (ntkrnlmp md5 9cfaaf0f), writes a fresh MBR/VBR
(BootLoaderLocation=2 -> restores 55AA), reboots.

Stage 1: boot CD, run text-mode install to the reboot (qemu -no-reboot exits).
Stage 2: boot HD repeatedly across the 2nd-stage + GuiRunOnce reboots until a
stable session/desktop, bailing on any crash.
"""
import sys, time, os, subprocess
sys.path.insert(0, '/home/kreijstal/git/reactos')
from ros_test import RosQemu, _qemu_pids_for_disk

DISK = '/home/kreijstal/git/reactos/reactos.qcow2'
ISO  = '/home/kreijstal/git/reactos/build_nt10/bootcdregtest.iso'
os.environ.setdefault('DISPLAY', ':10.0')

q = RosQemu(name='rosfmt', iso=ISO, disk=DISK)

def launch(cd):
    q._mon = None
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
        cmd = base + ['-drive', f'file={DISK},format=qcow2,if=ide',
                      '-cdrom', ISO, '-boot', 'd']
    else:
        cmd = base + ['-drive', f'file={DISK},format=qcow2,if=ide', '-boot', 'c']
    subprocess.run(cmd, check=True, capture_output=True)
    time.sleep(0.5)
    pids = _qemu_pids_for_disk(DISK)
    q.pid = pids[0] if pids else None
    print(f"  launched cd={cd} pid={q.pid}", flush=True)

def tail(n=15):
    for l in q.log_lines()[-n:]:
        print("   |", l.rstrip(), flush=True)

CRASH = ('kdb:', 'Assertion', 'BugCheck', '*** STOP')

# ---- Stage 1 -----------------------------------------------------------------
print("=== STAGE 1: unattended FORMAT install (boot CD) ===", flush=True)
launch(cd=True)
t0 = time.time()
last_pct = ''
done = False
while time.time() - t0 < 2400:
    if not q.is_alive():
        print(f"  qemu exited (reboot) at {time.time()-t0:.0f}s", flush=True)
        done = True
        break
    if any(any(c in l for c in CRASH) for l in q.log_lines()[-40:]):
        print(f"  CRASH during install at {time.time()-t0:.0f}s:", flush=True)
        tail(20); sys.exit(2)
    lines = q.log_lines()
    pcts = [l for l in lines if 'PROGRESS:' in l]
    if pcts and pcts[-1] != last_pct:
        last_pct = pcts[-1]
        print(f"  {last_pct.strip()[-60:]} ({time.time()-t0:.0f}s)", flush=True)
    # nudge toward reboot if at the success/shutdown path
    if any('SETUP_PAGE:SUCCESS' in l or 'Install bootcode' in l
           or 'NtfsShutdown' in l for l in lines[-60:]):
        q.send('ret')
    time.sleep(3)
if not done:
    print("  STAGE 1 stalled:", flush=True); tail(30); sys.exit(3)

# ---- Stage 2: ride out 2nd-stage + GuiRunOnce reboots ------------------------
print("=== STAGE 2: boot installed HD across reboots ===", flush=True)
for attempt in range(1, 9):
    print(f"--- HD boot {attempt} ---", flush=True)
    launch(cd=False)
    bstart = time.time()
    outcome = None
    stable_since = None
    while time.time() - bstart < 300:
        if any(any(c in l for c in CRASH) for l in q.log_lines()[-40:]):
            print(f"  CRASH on HD boot {attempt}:", flush=True); tail(20)
            outcome = 'crash'; break
        if not q.is_alive():
            print(f"  qemu exited (reboot) at {time.time()-bstart:.0f}s", flush=True)
            outcome = 'reboot'; break
        lines = q.log_lines()
        if any('Session 0 is ready' in l or 'Explorer' in l or 'AUTORUN' in l
               or 'serial-shell' in l or 'regtest' in l.lower() for l in lines[-60:]):
            if stable_since is None:
                stable_since = time.time()
                print(f"  session marker at {time.time()-bstart:.0f}s; settling", flush=True)
        # consider it up if a session marker held and the box stayed alive 40s
        if stable_since and time.time() - stable_since > 40:
            outcome = 'desktop'; break
        time.sleep(2)
    if outcome == 'crash':
        sys.exit(4)
    if outcome == 'desktop':
        time.sleep(10)
        p, _ = q.screenshot('installed')
        print(f"  screenshot -> {p}", flush=True)
        tail(8)
        print("=== INSTALL_DONE: clean system installed and booting ===", flush=True)
        q.monitor_cmd('system_powerdown')
        for _ in range(60):
            if not q.is_alive():
                print("  clean powerdown", flush=True); break
            time.sleep(2)
        sys.exit(0)
    # reboot or timeout -> loop and boot HD again
print("=== exhausted HD boots without stable desktop ===", flush=True)
sys.exit(5)
