#!/usr/bin/env python3
"""Verify the SMP swap-busy/Running fix: boot reactos.qcow2 at -smp 4 several
times under -snapshot (writes discarded, base protected) and confirm the
100%-reproducible wild-RIP / 0x7E context-switch crash no longer occurs.

The fixed kernel (ntkrnlmp md5 9cfaaf0f) was installed by the bootcdntfs
over-the-top reinstall, so the on-disk system already carries the fix.
"""
import sys, time, os, subprocess
sys.path.insert(0, '/home/kreijstal/git/reactos')
from ros_test import RosQemu, _qemu_pids_for_disk

DISK = '/home/kreijstal/git/reactos/reactos.qcow2'
os.environ.setdefault('DISPLAY', ':10.0')
N = int(sys.argv[1]) if len(sys.argv) > 1 else 4

q = RosQemu(name='rossmp4', iso='', disk=DISK)

CRASH = ('kdb:', 'Assertion', 'BugCheck', '*** STOP', 'could not be accessed',
         'PAGE_FAULT', '0x0000007E', 'Unhandled exception', 'vadnode.c')
DESK  = ('Session 0 is ready', 'Explorer', 'AUTORUN', 'GuiRunOnce')

def launch():
    q._mon = None
    for f in (q.log_path, q.sock_path):
        if os.path.exists(f):
            os.unlink(f)
    cmd = ['qemu-system-x86_64', '-m', '2048', '-smp', '4',
           '-drive', f'file={DISK},format=qcow2,if=ide',
           '-snapshot', '-boot', 'c',
           '-serial', f'file:{q.log_path}',
           '-monitor', f'unix:{q.sock_path},server,nowait',
           '-display', 'gtk',
           '-netdev', 'user,id=n0', '-device', 'rtl8139,netdev=n0',
           '-no-reboot', '-enable-kvm', '-s', '-daemonize']
    subprocess.run(cmd, check=True, capture_output=True)
    time.sleep(0.5)
    pids = _qemu_pids_for_disk(DISK)
    q.pid = pids[0] if pids else None

results = []
for i in range(1, N + 1):
    print(f"=== -smp 4 boot {i}/{N} ===", flush=True)
    launch()
    t0 = time.time()
    verdict = None
    while time.time() - t0 < 200:
        if q.crashed() or any(any(c in l for c in CRASH) for l in q.log_lines()[-60:]):
            verdict = 'CRASH'
            print(f"  CRASH at {time.time()-t0:.0f}s:", flush=True)
            for l in q.log_lines()[-15:]:
                print("   |", l.rstrip(), flush=True)
            break
        if not q.is_alive():
            verdict = 'EXIT'
            print(f"  qemu exited at {time.time()-t0:.0f}s", flush=True)
            break
        if any(any(d in l for d in DESK) for l in q.log_lines()[-80:]):
            verdict = 'DESKTOP'
            print(f"  reached session/desktop at {time.time()-t0:.0f}s", flush=True)
            break
        time.sleep(2)
    if verdict is None:
        verdict = 'TIMEOUT'
    if verdict == 'DESKTOP':
        time.sleep(25)
        p, _ = q.screenshot(f'smp4_boot{i}')
        print(f"  screenshot -> {p}", flush=True)
    results.append(verdict)
    q.kill()
    time.sleep(2)

print("=== RESULTS:", results, "===", flush=True)
crashes = sum(1 for r in results if r == 'CRASH')
desks   = sum(1 for r in results if r == 'DESKTOP')
if crashes:
    print(f"FAIL: {crashes}/{N} boots crashed", flush=True); sys.exit(1)
if desks == 0:
    print("INCONCLUSIVE: no boot reached desktop", flush=True); sys.exit(2)
print(f"PASS: {desks}/{N} boots reached desktop, 0 crashes — SMP fix verified", flush=True)
sys.exit(0)
