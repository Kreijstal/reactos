#!/usr/bin/env python3
"""Resume the nt10msys install from the bootloader-install page (qemu already
running at that screen). PID passed as argv[1]."""
import sys, time
sys.path.insert(0, '/home/kreijstal/git/reactos')
from ros_test import RosQemu, finish_install, boot_from_hd, drive_first_boot_wizard

PID = int(sys.argv[1])
q = RosQemu(name='nt10msys', iso='/home/kreijstal/git/reactos/build_nt10/bootcd.iso',
            disk='/tmp/nt10msys.img')
q.pid = PID

print("=== ENTER on bootloader page + finish_install -> reboot ===", flush=True)
try:
    finish_install(q, success_timeout=180)
except Exception as e:
    print(f"finish_install raised: {e!r}", flush=True)

print("=== first HD boot ===", flush=True)
booted = boot_from_hd(q)
print(f"boot_from_hd -> {booted}", flush=True)
if not booted:
    print("first HD boot failed", flush=True); sys.exit(2)

print("=== GUI first-boot wizard ===", flush=True)
wiz = drive_first_boot_wizard(q)
print(f"wizard -> {wiz}", flush=True)
q.kill()
print("=== install resume complete; disk /tmp/nt10msys.img ===", flush=True)
