#!/usr/bin/env python3
"""Cycle HD boots to maximize regtest/rosautotest process-creation churn and
catch the vadnode.c:82 VADASSERT. Install is already done. Auto-reboots when
churn settles to pack more bursts per hour. Stops on first VADASSERT capture."""
import sys, time, os, subprocess
sys.path.insert(0, '/home/kreijstal/git/reactos')
from ros_test import RosQemu, _qemu_pids_for_disk

DISK = '/home/kreijstal/git/reactos/reactos.qcow2'
ISO  = '/home/kreijstal/git/reactos/build_nt10/bootcdregtest.iso'
os.environ.setdefault('DISPLAY', ':10.0')
CAP  = '/home/kreijstal/git/reactos/vad_capture.log'
MASTER = '/home/kreijstal/git/reactos/vad_master.log'

q = RosQemu(name='vadcyc', iso=ISO, disk=DISK)

def hb(msg):
    print(f"[HB {time.strftime('%H:%M:%S')}] {msg}", flush=True)

def launch():
    q._mon = None
    for f in (q.log_path, q.sock_path):
        if os.path.exists(f):
            os.unlink(f)
    cmd = ['qemu-system-x86_64', '-m', '2048', '-smp', '4', '-name', 'vadcyc',
           '-serial', f'file:{q.log_path}',
           '-monitor', f'unix:{q.sock_path},server,nowait',
           '-display', 'none',
           '-netdev', 'user,id=n0', '-device', 'rtl8139,netdev=n0',
           '-enable-kvm', '-s', '-daemonize',
           '-drive', f'file={DISK},format=qcow2,if=ide', '-boot', 'c']
    subprocess.run(cmd, check=True, capture_output=True)
    time.sleep(0.5)
    pids = _qemu_pids_for_disk(DISK)
    q.pid = pids[0] if pids else None

def kill_qemu():
    for p in _qemu_pids_for_disk(DISK):
        try: os.kill(p, 15)
        except: pass
    time.sleep(2)

def capture():
    lines = [l for l in q.log_lines() if 'VADASSERT' in l]
    if lines:
        with open(CAP, 'a') as f:
            f.write(f"\n=== capture {time.strftime('%Y-%m-%d %H:%M:%S')} ===\n")
            for l in lines:
                f.write(l.rstrip() + "\n")
    return len(lines)

def churn_count():
    return sum(1 for l in q.log_lines() if 'apitest' in l or "_test'" in l)

total_modules = set()
for boot in range(1, 200):
    hb(f"--- boot {boot} (distinct test modules seen={len(total_modules)}) ---")
    launch()
    bstart = time.time()
    last_churn_t = time.time()
    last_churn_n = 0
    settled_logged = False
    while time.time() - bstart < 360:
        if not q.is_alive():
            hb(f"qemu exited at {time.time()-bstart:.0f}s")
            break
        lines = q.log_lines()
        if any('VADASSERT' in l for l in lines):
            n = capture()
            hb(f"*** VADASSERT CAUGHT boot {boot} ({n} lines) -> {CAP} ***")
            for l in lines[-50:]:
                print("   |", l.rstrip(), flush=True)
            # append boot log to master and stop
            with open(MASTER, 'a') as f:
                f.write(f"\n===== boot {boot} (VADASSERT) =====\n")
                f.writelines(l for l in lines)
            sys.exit(0)
        for l in lines:
            if 'apitest' in l:
                # crude module name extraction
                i = l.find("'")
                if i >= 0:
                    total_modules.add(l[i+1:l.find("'", i+1)])
        # churn-settle detection: if test churn started then stalled 30s, reboot
        n = churn_count()
        if n > last_churn_n:
            last_churn_n = n
            last_churn_t = time.time()
        if last_churn_n > 5 and (time.time() - last_churn_t) > 30:
            if not settled_logged:
                hb(f"  churn settled ({last_churn_n} test lines) at {time.time()-bstart:.0f}s; rebooting")
                settled_logged = True
            kill_qemu()
            break
        time.sleep(2)
    # archive this boot's serial
    try:
        with open(MASTER, 'a') as f:
            f.write(f"\n===== boot {boot} =====\n")
            f.writelines(q.log_lines())
    except Exception:
        pass
    kill_qemu()
hb(f"exhausted boots; distinct modules={len(total_modules)}")
