#!/usr/bin/env python3
#
# PROJECT:     ReactOS host-side test runner
# LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
# PURPOSE:     Boot a ReactOS VM, run a selected test suite/group, report results.
#
# This is the single host-side entry point that drives both in-guest test
# runners (kmtcdrunner for kernel-mode kmtests, rosautotest for the user-mode
# winetests) through a pluggable VM backend (QEMU by default; VirtualBox also
# wired).  It is what the CTest entries in CMakeLists.txt invoke.
#
# Examples:
#   rostest.py --suite kmtest   --test Example  --kmtest-img build/kmtest.img
#   rostest.py --suite rostests --test gdi32:bitmap --image reactos.qcow2
#   rostest.py --suite rostests --test comctl32:imagelist --image reactos.qcow2 --fresh
#   rostest.py --suite all --image reactos.qcow2 --kmtest-img build/kmtest.img
#
# Exit code is 0 only if every selected group ran and reported 0 failures.

import os
import sys
import signal
import argparse
import tempfile
from dataclasses import dataclass

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from backends import VmBackend                       # noqa: E402
import suites                                         # noqa: E402


@dataclass
class Config:
    suite: str
    test: str
    backend: str
    image: str
    image_format: str
    iso: str
    kmtest_img: str
    fresh: bool
    timeout: int
    install_timeout: int
    max_boots: int
    memory: int
    smp: int
    kvm: bool
    display: str
    workdir: str
    tag: str


def parse_args(argv):
    p = argparse.ArgumentParser(
        prog="rostest",
        description="Run ReactOS kmtest / rostests in a VM and report pass/fail.")
    p.add_argument("--suite", choices=["kmtest", "rostests", "all"], default="all")
    p.add_argument("--test", default="",
                   help="select one group: a kmtest name, or 'module' / 'module:test' for rostests")
    p.add_argument("--backend", default="qemu", choices=["qemu", "virtualbox"])
    p.add_argument("--image", default=os.environ.get("ROSTEST_IMAGE", ""),
                   help="installed system disk to (re)use for rostests")
    p.add_argument("--image-format", default="qcow2")
    p.add_argument("--iso", default=os.environ.get("ROSTEST_ISO", ""),
                   help="bootcdregtest ISO, used by --fresh")
    p.add_argument("--kmtest-img", default=os.environ.get("ROSTEST_KMTEST_IMG", ""),
                   help="bootable kmtest image")
    p.add_argument("--fresh", action="store_true",
                   help="reinstall the system image before running rostests")
    p.add_argument("--timeout", type=int, default=2400, help="per-suite run cap (s)")
    p.add_argument("--install-timeout", type=int, default=2400)
    p.add_argument("--max-boots", type=int, default=8,
                   help="rostests: GuiRunOnce/Run-key reboots to ride")
    p.add_argument("--memory", type=int, default=2048)
    p.add_argument("--smp", type=int, default=1)
    p.add_argument("--no-kvm", action="store_true")
    p.add_argument("--display", default="none", choices=["none", "gtk"])
    p.add_argument("--workdir", default="",
                   help="scratch dir for copies/logs (default: a temp dir)")
    return p.parse_args(argv)


def main(argv=None):
    args = parse_args(sys.argv[1:] if argv is None else argv)

    workdir = args.workdir or tempfile.mkdtemp(prefix="rostest-")
    os.makedirs(workdir, exist_ok=True)
    tag = str(os.getpid())

    cfg = Config(
        suite=args.suite, test=args.test, backend=args.backend,
        image=args.image, image_format=args.image_format, iso=args.iso,
        kmtest_img=args.kmtest_img, fresh=args.fresh, timeout=args.timeout,
        install_timeout=args.install_timeout, max_boots=args.max_boots,
        memory=args.memory, smp=args.smp, kvm=not args.no_kvm,
        display=args.display, workdir=workdir, tag=tag)

    backend = VmBackend.create(args.backend)

    # On Ctrl-C / SIGTERM, tear down any VM we launched before exiting, so a
    # daemonized guest never outlives the tool holding a disk-image write lock.
    def _terminate(signum, _frame):
        backend.stop_all()
        sys.exit(130 if signum == signal.SIGINT else 143)
    for _sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(_sig, _terminate)

    results = []
    try:
        if args.suite in ("kmtest", "all"):
            results.append(suites.run_kmtest(backend, cfg))
        if args.suite in ("rostests", "all"):
            results.append(suites.run_rostests(backend, cfg))
    finally:
        backend.stop_all()

    print("\n" + "=" * 64)
    print(f"ROSTEST SUMMARY (backend={backend.name}, workdir={workdir})")
    overall_ok = True
    for r in results:
        if r.crashed or not r.groups:
            print(f"  {r.suite:9s} : DID NOT COMPLETE ({r.note or 'no results'})")
            overall_ok = False
            continue
        for g in r.groups:
            status = "PASS" if (g.failures == 0 and not g.errored) else "FAIL"
            extra = " ERRORED" if g.errored else ""
            print(f"  {r.suite:9s} : {g.name:24s} {status} "
                  f"(exec={g.executed} fail={g.failures} skip={g.skipped}){extra}")
            if status == "FAIL":
                overall_ok = False
        print(f"  {r.suite:9s} : total failures = {r.total_failures}")
    print("=" * 64)
    print("RESULT:", "PASS" if overall_ok else "FAIL")
    return 0 if overall_ok else 1


if __name__ == "__main__":
    sys.exit(main())
