#!/usr/bin/env python3
"""Reinstall reactos.qcow2 from instrumented bootcdregtest.iso, then ride the
regtest reboot churn capturing VADASSERT forensic lines (vadnode.c:82 producer)."""
import sys, time, os, subprocess
sys.path.insert(0, '/home/kreijstal/git/reactos')
from ros_test import RosQemu, _qemu_pids_for_disk

DISK = '/home/kreijstal/git/reactos/reactos.qcow2'
ISO  = '/home/kreijstal/git/reactos/build_nt10/bootcdregtest.iso'
os.environ.setdefault('DISPLAY', ':10.0')
CAP  = '/home/kreijstal/git/reactos/vad_capture.log'

q = RosQemu(name='vadrepro', iso=ISO, disk=DISK)

def hb(msg):
    print(f"[HB {time.strftime('%H:%M:%S')}] {msg}", flush=True)

def zero_sig():
    subprocess.run(['qemu-io', '-c', 'write -P 0 510 2', '-f', 'qcow2', DISK],
                   check=True, capture_output=True)
    hb("zeroed MBR 55AA -> CD will boot")

def launch(cd):
    q._mon = None
    for f in (q.log_path, q.sock_path):
        if os.path.exists(f):
            os.unlink(f)
    base = ['qemu-system-x86_64', '-m', '2048',
            '-name', 'vadrepro',
            '-serial', f'file:{q.log_path}',
            '-monitor', f'unix:{q.sock_path},server,nowait',
            '-display', 'none',
            '-netdev', 'user,id=n0', '-device', 'rtl8139,netdev=n0',
            '-enable-kvm', '-s', '-daemonize']
    if cd:
        cmd = base + ['-no-reboot',
                      '-drive', f'file={DISK},format=qcow2,if=ide',
                      '-cdrom', ISO, '-boot', 'd']
    else:
        cmd = base + ['-drive', f'file={DISK},format=qcow2,if=ide', '-boot', 'c']
    subprocess.run(cmd, check=True, capture_output=True)
    time.sleep(0.5)
    pids = _qemu_pids_for_disk(DISK)
    q.pid = pids[0] if pids else None

def tail(n=15):
    for l in q.log_lines()[-n:]:
        print("   |", l.rstrip(), flush=True)

def capture_vadassert():
    """Scan log for VADASSERT lines, append new ones to CAP. Return True if any seen this boot."""
    seen = False
    lines = [l for l in q.log_lines() if 'VADASSERT' in l]
    if lines:
        with open(CAP, 'a') as f:
            f.write(f"\n=== capture {time.strftime('%Y-%m-%d %H:%M:%S')} ===\n")
            for l in lines:
                f.write(l.rstrip() + "\n")
        seen = True
    return seen, len(lines)

# ---- STAGE 1: format install -------------------------------------------------
hb("=== STAGE 1: unattended FORMAT install ===")
zero_sig()
launch(cd=True)
t0 = time.time()
last_pct = ''
done = False
while time.time() - t0 < 2400:
    if not q.is_alive():
        hb(f"qemu exited (install reboot) at {time.time()-t0:.0f}s")
        done = True
        break
    lines = q.log_lines()
    if any('VADASSERT' in l for l in lines):
        capture_vadassert()
        hb("VADASSERT during INSTALL -- captured")
    pcts = [l for l in lines if 'PROGRESS:' in l]
    if pcts and pcts[-1] != last_pct:
        last_pct = pcts[-1]
        hb(f"install {last_pct.strip()[-50:]} ({time.time()-t0:.0f}s)")
    if any('SETUP_PAGE:SUCCESS' in l or 'Install bootcode' in l
           or 'NtfsShutdown' in l for l in lines[-60:]):
        q.send('ret')
    time.sleep(4)
if not done:
    hb("STAGE 1 stalled"); tail(30); sys.exit(3)

# ---- STAGE 2: ride regtest churn ---------------------------------------------
hb("=== STAGE 2: ride regtest reboot churn, capture VADASSERT ===")
total_caught = 0
summary_modules = set()
for boot in range(1, 40):
    hb(f"--- HD boot {boot} (caught so far={total_caught}, modules={len(summary_modules)}) ---")
    launch(cd=False)
    bstart = time.time()
    last_hb = time.time()
    while time.time() - bstart < 600:
        if not q.is_alive():
            hb(f"qemu exited (reboot) at {time.time()-bstart:.0f}s")
            break
        lines = q.log_lines()
        if any('VADASSERT' in l for l in lines):
            seen, n = capture_vadassert()
            total_caught += 1
            hb(f"*** VADASSERT CAUGHT on boot {boot} ({n} lines) -> {CAP} ***")
            tail(40)
            # KDB has the box; grab a backtrace too
            time.sleep(1)
            q.monitor_cmd("sendkey ret")
            time.sleep(60)  # leave for inspection
            sys.exit(0)
        for l in lines:
            if 'test_summary' in l.lower() or ': tests:' in l.lower():
                summary_modules.add(l.strip()[:40])
        if time.time() - last_hb > 90:
            last_hb = time.time()
            tl = q.log_lines()
            hb(f"  boot {boot} alive {time.time()-bstart:.0f}s, last: {tl[-1].rstrip()[-60:] if tl else '(none)'}")
        time.sleep(3)
    # reboot or timeout -> next boot
hb(f"=== exhausted boots; caught={total_caught} modules={len(summary_modules)} ===")
