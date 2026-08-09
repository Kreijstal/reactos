#!/usr/bin/env python3
"""Verify the MM page-double-use fix (MmCheckDirtySegment SystemMapCount guard).

Boots reactos.qcow2 (fixed ntkrnlmp injected) at -smp 4 (MP) + NIC under
-snapshot, with LOW guest RAM (-m 512) to keep the balancer/working-set
trimmer running constantly. That is the producer path: the trimmer
(MmPageOutPhysicalAddress -> MmCheckDirtySegment) used to free a clean
SHARE_COUNT==0 VACB page while a Cc system-space view still mapped it (no
rmap), causing rmap-dup ASSERT / 0x23 CcMapData / Bad PTE. The fix refuses to
free when Segment->SystemMapCount > 0. Soak at desktop so the trimmer churns
the file cache under pressure on multiple CPUs.
"""
import sys, time, os, subprocess
sys.path.insert(0, '/home/kreijstal/git/reactos')
from ros_test import RosQemu, _qemu_pids_for_disk

DISK = '/home/kreijstal/git/reactos/reactos.qcow2'
os.environ.setdefault('DISPLAY', ':10.0')
N    = int(sys.argv[1]) if len(sys.argv) > 1 else 3
MEM  = sys.argv[2] if len(sys.argv) > 2 else '512'
SOAK = int(sys.argv[3]) if len(sys.argv) > 3 else 150

q = RosQemu(name='rosmm', iso='', disk=DISK)

# page-double-use family + generic fatal markers. NOTE: the benign startup
# line "KDB: Executing KDBinit file..." is NOT a crash; a real KDB break is
# the "kdb:>" prompt or "Entered debugger".
CRASH = ('kdb:>', 'Entered debugger', 'Assertion \'', 'Assertion failed',
         '*** Fatal', '*** STOP', 'BugCheck', 'KeBugCheck',
         'released with rmaps', 'Bad PTE', 'PAGE_FAULT_IN_NONPAGED',
         '0x00000023', 'MEMORY_MANAGEMENT', 'ARM3 detected')
DESK  = ('Session 0 is ready', 'explorer', 'Explorer', 'AUTORUN',
         'GuiRunOnce', 'Tcpip', 'DllInitialize')

def launch():
    q._mon = None
    for f in (q.log_path, q.sock_path):
        if os.path.exists(f):
            os.unlink(f)
    cmd = ['qemu-system-x86_64', '-m', MEM, '-smp', '4',
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

def crash_hit():
    for l in q.log_lines()[-80:]:
        for c in CRASH:
            if c in l:
                return l.rstrip()
    return None

results = []
for i in range(1, N + 1):
    print(f"=== MP({MEM}MB) boot {i}/{N} ===", flush=True)
    launch()
    t0 = time.time()
    verdict = None
    reached_desk_at = None
    while time.time() - t0 < 240:
        hit = crash_hit()
        if q.crashed() or hit:
            verdict = 'CRASH'
            print(f"  CRASH at {time.time()-t0:.0f}s: {hit}", flush=True)
            for l in q.log_lines()[-20:]:
                print("   |", l.rstrip(), flush=True)
            break
        if not q.is_alive():
            verdict = 'EXIT'
            print(f"  qemu exited at {time.time()-t0:.0f}s", flush=True)
            break
        if reached_desk_at is None and any(any(d in l for d in DESK) for l in q.log_lines()[-120:]):
            reached_desk_at = time.time()
            print(f"  reached session at {reached_desk_at-t0:.0f}s; soaking {SOAK}s under trim pressure", flush=True)
        if reached_desk_at is not None and time.time() - reached_desk_at > SOAK:
            verdict = 'DESKTOP_SOAKED'
            print(f"  soaked {SOAK}s with no crash", flush=True)
            break
        time.sleep(2)
    if verdict is None:
        # survived the full window alive with no crash marker -> pass
        verdict = 'SURVIVED' if q.is_alive() else 'EXIT'
    if verdict in ('DESKTOP_SOAKED', 'SURVIVED'):
        try:
            p, _ = q.screenshot(f'mm_boot{i}')
            print(f"  screenshot -> {p}", flush=True)
        except Exception as e:
            print(f"  screenshot failed: {e}", flush=True)
    results.append(verdict)
    q.kill()
    time.sleep(2)

print("=== RESULTS:", results, "===", flush=True)
crashes = sum(1 for r in results if r == 'CRASH')
oks     = sum(1 for r in results if r in ('DESKTOP_SOAKED', 'SURVIVED'))
if crashes:
    print(f"FAIL: {crashes}/{N} boots crashed (MM page-double-use)", flush=True); sys.exit(1)
if oks == 0:
    print("INCONCLUSIVE: no boot reached a soakable desktop", flush=True); sys.exit(2)
print(f"PASS: {oks}/{N} MP boots soaked {SOAK}s under trim pressure, 0 page-reuse crashes", flush=True)
sys.exit(0)
