#!/usr/bin/env python3
"""Fresh unattended NTFS reinstall onto reactos.qcow2, forcing the installer to
run WITHOUT editing the target disk: attach the isohybrid bootcdntfs.iso as the
first IDE hard disk (drive 80h) so SeaBIOS boots it (isombr -> isoboot
start_hybrid: DriveNumber==80h -> boot FREELDR/usetup directly), and the target
reactos.qcow2 as the second IDE disk (disk 1, = DestinationDiskNumber).

Stage 1: ISO(disk0)+qcow2(disk1), boot the ISO -> usetup formats disk1 NTFS,
installs the fixed SMP kernel, writes a fresh MBR/VBR to disk1, reboots.
Stage 2: qcow2 ONLY (ISO detached) -> boots the installed system to desktop.
Carries the cross-CPU idle-wakeup fix (ntkrnlmp md5 19561ee3).
"""
import sys, time, os, subprocess
sys.path.insert(0, '/home/kreijstal/git/reactos')
from ros_test import RosQemu, _qemu_pids_for_disk

DISK = '/home/kreijstal/git/reactos/reactos.qcow2'
ISO  = '/home/kreijstal/git/reactos/build_nt10/bootcdntfs.iso'
os.environ.setdefault('DISPLAY', ':10.0')
q = RosQemu(name='rosisohyb', iso=ISO, disk=DISK)

def launch(cd):
    q._mon = None
    for f in (q.log_path, q.sock_path):
        if os.path.exists(f):
            os.unlink(f)
    base = ['qemu-system-x86_64', '-m', '2048', '-smp', '4',
            '-serial', f'file:{q.log_path}',
            '-monitor', f'unix:{q.sock_path},server,nowait',
            '-display', 'gtk',
            '-netdev', 'user,id=n0', '-device', 'rtl8139,netdev=n0',
            '-no-reboot', '-enable-kvm', '-s', '-daemonize']
    if cd:
        # ISO as first IDE HD (80h) -> isohybrid boots usetup; qcow2 as disk 1.
        cmd = base + ['-drive', f'file={ISO},format=raw,if=ide,index=0',
                      '-drive', f'file={DISK},format=qcow2,if=ide,index=1',
                      '-boot', 'c']
    else:
        cmd = base + ['-drive', f'file={DISK},format=qcow2,if=ide,index=0',
                      '-boot', 'c']
    subprocess.run(cmd, check=True, capture_output=True)
    time.sleep(0.5)
    pids = _qemu_pids_for_disk(DISK)
    q.pid = pids[0] if pids else None
    print(f"  launched cd={cd} pid={q.pid}", flush=True)

def tail(n=15):
    for l in q.log_lines()[-n:]:
        print("   |", l.rstrip()[:120], flush=True)

CRASH = ('Assertion', 'BugCheck', '*** STOP', 'KMODE_EXCEPTION')

# ---- Stage 1 ----------------------------------------------------------------
print("=== STAGE 1: unattended NTFS install (ISO as HD) ===", flush=True)
launch(cd=True)
t0 = time.time(); done = False; lastmark = ''
while time.time() - t0 < 2400:
    if not q.is_alive():
        print(f"  qemu exited (install reboot) at {time.time()-t0:.0f}s", flush=True)
        done = True; break
    lines = q.log_lines()
    if any(any(c in l for c in CRASH) for l in lines[-40:]):
        print(f"  CRASH during install at {time.time()-t0:.0f}s:", flush=True); tail(25); sys.exit(2)
    marks = [l for l in lines if any(k in l for k in
             ('FormatPartition', 'Formatting', 'FileCopy', 'Copying', 'Registry',
              'Install NTFS bootcode', 'NtfsShutdown', 'SETUP_PAGE', 'too small',
              'DestinationDisk', 'usetup.c'))]
    if marks and marks[-1] != lastmark:
        lastmark = marks[-1]; print(f"  [{time.time()-t0:.0f}s] {lastmark.strip()[-90:]}", flush=True)
    if any('NtfsShutdown' in l or 'Install NTFS bootcode' in l or 'SETUP_PAGE:SUCCESS' in l
           for l in lines[-80:]):
        q.send('ret')
    time.sleep(4)
if not done:
    print("  STAGE 1 stalled. tail:", flush=True); tail(30); sys.exit(3)

# ---- Stage 2 ----------------------------------------------------------------
print("=== STAGE 2: boot installed qcow2 (ISO detached) ===", flush=True)
for attempt in range(1, 9):
    print(f"--- HD boot {attempt} ---", flush=True)
    launch(cd=False)
    bstart = time.time(); outcome = None; stable_since = None
    while time.time() - bstart < 300:
        lines = q.log_lines()
        if any(any(c in l for c in CRASH) for l in lines[-40:]):
            print(f"  CRASH on HD boot {attempt}:", flush=True); tail(25); outcome = 'crash'; break
        if not q.is_alive():
            print(f"  qemu exited (reboot) at {time.time()-bstart:.0f}s", flush=True); outcome = 'reboot'; break
        if any('serial-shell' in l or 'Explorer' in l or 'AUTORUN' in l
               or 'Session 0 is ready' in l for l in lines[-80:]):
            if stable_since is None:
                stable_since = time.time(); print(f"  session marker at {time.time()-bstart:.0f}s; settling", flush=True)
        if stable_since and time.time() - stable_since > 30:
            outcome = 'desktop'; break
        time.sleep(2)
    if outcome == 'crash':
        sys.exit(4)
    if outcome == 'desktop':
        time.sleep(8)
        p, _ = q.screenshot('isohyb_installed')
        print(f"  screenshot -> {p}", flush=True); tail(6)
        print("=== INSTALL_DONE: NTFS system with fixed kernel booting ===", flush=True)
        q.monitor_cmd('system_powerdown')
        for _ in range(60):
            if not q.is_alive():
                print("  clean powerdown", flush=True); break
            time.sleep(2)
        sys.exit(0)
print("=== exhausted HD boots ===", flush=True); sys.exit(5)
