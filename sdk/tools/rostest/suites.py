#!/usr/bin/env python3
#
# PROJECT:     ReactOS host-side test runner
# LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
# PURPOSE:     Suite logic for the two ReactOS in-guest test runners.
#
# kmtest  -- boots the bootable kmtest image, lets kmtcdrunner run the kernel
#            tests, and harvests the KMTCD-* serial markers it emits.
# rostests -- boots an installed system (or freshly installs one) and lets
#            rosautotest run the user-mode winetests, parsing the per-group
#            "N tests executed (M todo, K failures)" summary lines.
#
# Both suites can be steered to a single test via a small FAT "control disk"
# the host writes (see controldisk.py); when the optional in-guest hooks are not
# present, the rostests suite falls back to a fresh install whose baked-in
# command runs the requested group.

import os
import re
import time
import shutil
import subprocess
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

from backends import VmBackend, VmSpec
import controldisk


# A winetest group summary line, e.g.
#   bitmap: 18676 tests executed (0 marked as todo, 1185 failures), 0 skipped.
SUMMARY_RE = re.compile(
    r"([A-Za-z0-9_]+): (\d+) tests? executed "
    r"\((\d+) marked as todo, (\d+) failures?\)(?:, (\d+) skipped)?")

# kmtcdrunner end marker, e.g.  KMTCD-END Example exit=0 failures=0
KMTCD_END_RE = re.compile(r"KMTCD-END\s+(\S+)\s+exit=(-?\d+)\s+failures=(-?\d+)")

# "*** Assertion failed:" is deliberately NOT a crash signature: on a KDBG
# build a kernel ASSERT raises STATUS_ASSERTION_FAILURE into the caller and the
# guest keeps running; only an actual debugger entry ("Entered debugger",
# "kdb:>") or a bugcheck stops it.
CRASH_SIGS = ("*** STOP:", "*** Fatal System Error",
              "Entered debugger", "kdb:>")


@dataclass
class GroupResult:
    name: str
    executed: int = 0
    todo: int = 0
    failures: int = 0
    skipped: int = 0
    errored: bool = False        # crashed / non-zero exit without a failure count


@dataclass
class SuiteResult:
    suite: str
    groups: List[GroupResult] = field(default_factory=list)
    crashed: bool = False
    note: str = ""

    @property
    def total_failures(self) -> int:
        return sum(g.failures for g in self.groups)

    @property
    def passed(self) -> bool:
        return (not self.crashed and bool(self.groups)
                and all(g.failures == 0 and not g.errored for g in self.groups))


def _read(path: str) -> str:
    try:
        with open(path, "rb") as f:
            return f.read().decode("latin-1", "replace")
    except FileNotFoundError:
        return ""


def _log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def _wait(backend: VmBackend, spec: VmSpec, deadline: float,
          done, heartbeat=None) -> str:
    """Poll until done(text) is truthy, the VM exits, a crash signature shows,
    or the deadline passes.  Returns 'done' | 'off' | 'crash' | 'timeout'."""
    com1 = spec.serial_files[0] if spec.serial_files else None
    last_hb = time.time()
    while time.time() < deadline:
        text = "".join(_read(f) for f in spec.serial_files)
        if done(text):
            return "done"
        if not backend.is_alive(spec):
            return "off"
        if com1:
            tail = _read(com1).splitlines()[-60:]
            if any(sig in line for line in tail for sig in CRASH_SIGS):
                return "crash"
        if heartbeat and time.time() - last_hb >= 120:
            heartbeat(text)
            last_hb = time.time()
        time.sleep(5)
    return "timeout"


# --------------------------------------------------------------------------- #
#  NTFS data disk (kmtest NtfsDirIndex/FatPerf want a writable NTFS volume)
# --------------------------------------------------------------------------- #
def make_ntfs_data_disk(path: str) -> Optional[str]:
    """Build a 256 MB MBR-partitioned NTFS data disk if the host tools exist.
    Returns the path, or None when mkntfs/sfdisk are unavailable (the affected
    kmtests then simply report 'no writable NTFS volume')."""
    if os.path.exists(path):
        return path
    if not (shutil.which("mkntfs") and shutil.which("sfdisk")):
        return None
    part = path + ".part"
    try:
        subprocess.run(["truncate", "-s", "256M", path], check=True)
        subprocess.run(["sfdisk", path], check=True, capture_output=True,
                       input=b"label: dos\nstart=2048, size=522240, type=7, bootable\n")
        subprocess.run(["truncate", "-s", str(522240 * 512), part], check=True)
        subprocess.run(["mkntfs", "-F", "-Q", "-L", "ROSTESTNTFS", part],
                       check=True, capture_output=True)
        subprocess.run(["dd", f"if={part}", f"of={path}", "bs=512", "seek=2048",
                        "conv=notrunc"], check=True, capture_output=True)
    finally:
        if os.path.exists(part):
            os.unlink(part)
    return path


# --------------------------------------------------------------------------- #
#  kmtest
# --------------------------------------------------------------------------- #
def run_kmtest(backend: VmBackend, cfg) -> SuiteResult:
    res = SuiteResult("kmtest")
    if not cfg.kmtest_img or not os.path.exists(cfg.kmtest_img):
        res.note = f"kmtest image not found: {cfg.kmtest_img}"
        res.crashed = True
        return res

    work = os.path.join(cfg.workdir, "kmtest.img")
    _log(f"copying {cfg.kmtest_img} -> {work}")
    shutil.copyfile(cfg.kmtest_img, work)

    disks = [(work, "raw", "ide")]
    ntfs = make_ntfs_data_disk(os.path.join(cfg.workdir, "kmtest_ntfsdata.img"))
    if ntfs:
        disks.append((ntfs, "raw", "ide"))

    # A single kmtest name selects one test via the control disk; empty = all.
    ctrl = None
    if cfg.test:
        ctrl = controldisk.write(os.path.join(cfg.workdir, "kmtest_ctrl.img"),
                                 controldisk.KMTEST_FILE, cfg.test.strip())
        if ctrl:
            disks.append((ctrl, "raw", "ide"))

    com1 = os.path.join(cfg.workdir, "kmtest-com1.log")
    com2 = os.path.join(cfg.workdir, "kmtest-com2.log")
    spec = VmSpec(name=f"rostest-kmtest-{cfg.tag}", disks=disks, boot_order="c",
                  serial_files=[com1, com2], memory_mb=cfg.memory, smp=cfg.smp,
                  kvm=cfg.kvm, display=cfg.display, debug_exit_port=0xF4,
                  monitor_sock=os.path.join(cfg.workdir, "kmtest-mon.sock"))

    _log(f"booting kmtest ({'all tests' if not cfg.test else cfg.test})")
    backend.launch(spec)

    def done(text):
        return "KMTCD-SUMMARY" in text
    def hb(text):
        ends = len(KMTCD_END_RE.findall(text))
        _log(f"  kmtest running: {ends} test(s) finished so far")

    outcome = _wait(backend, spec, time.time() + cfg.timeout, done, hb)
    text = "".join(_read(f) for f in spec.serial_files)
    backend.stop(spec)

    seen = set()
    for m in KMTCD_END_RE.finditer(text):
        name, exitc, fails = m.group(1), int(m.group(2)), int(m.group(3))
        if name in seen:
            continue
        seen.add(name)
        res.groups.append(GroupResult(name=name, failures=max(fails, 0),
                                      errored=(exitc != 0)))
    if outcome == "crash":
        res.crashed = True
        res.note = "crash/KDB signature on COM1"
    elif outcome == "timeout":
        res.crashed = True
        res.note = "timed out before KMTCD-SUMMARY"
    return res


# --------------------------------------------------------------------------- #
#  rostests
# --------------------------------------------------------------------------- #
def _fresh_install(backend: VmBackend, cfg) -> bool:
    """Zero the 55AA signature and boot the install ISO to format-install."""
    if not cfg.iso or not os.path.exists(cfg.iso):
        _log(f"--fresh requested but ISO not found: {cfg.iso}")
        return False
    if not hasattr(backend, "zero_boot_signature"):
        _log("backend cannot perform a fresh install; need a pre-installed image")
        return False
    _log("fresh install: zeroing 55AA + booting install ISO")
    backend.zero_boot_signature(cfg.image)
    com1 = os.path.join(cfg.workdir, "install-com1.log")
    spec = VmSpec(name=f"rostest-install-{cfg.tag}",
                  disks=[(cfg.image, cfg.image_format, "ide")], cdrom=cfg.iso,
                  boot_order="d", serial_files=[com1], memory_mb=cfg.memory,
                  smp=cfg.smp, kvm=cfg.kvm, display=cfg.display)
    backend.launch(spec)
    # The unattended install reboots (VM powers off due to -no-reboot) when done.
    outcome = _wait(backend, spec, time.time() + cfg.install_timeout,
                    done=lambda _t: False,
                    heartbeat=lambda _t: _log("  installing..."))
    backend.stop(spec)
    if outcome not in ("off", "done"):
        _log(f"install did not complete cleanly ({outcome})")
        return False
    _log("install reboot detected")
    return True


def run_rostests(backend: VmBackend, cfg) -> SuiteResult:
    res = SuiteResult("rostests")
    if not cfg.image or not os.path.exists(cfg.image):
        res.note = f"system image not found: {cfg.image}"
        res.crashed = True
        return res

    if cfg.fresh:
        if not _fresh_install(backend, cfg):
            res.crashed = True
            res.note = "fresh install failed"
            return res

    # Select the group(s) via the control disk read by regtest.cmd; absent ->
    # the installed default command runs.
    disks = [(cfg.image, cfg.image_format, "ide")]
    if cfg.test:
        module, _, test = cfg.test.partition(":")
        cmd = f"rosautotest /n /s {module} {test}".strip()
        ctrl = controldisk.write(os.path.join(cfg.workdir, "rostests_ctrl.img"),
                                 controldisk.ROSTESTS_FILE, cmd, batch=True)
        if ctrl:
            disks.append((ctrl, "raw", "ide"))

    com1 = os.path.join(cfg.workdir, "rostests-com1.log")
    # rosautotest prints one summary line per *sub-test*, keyed by the sub-test
    # name (e.g. "imagelist: ... failures" for comctl32:imagelist).  When a
    # specific sub-test was requested ("module:test") wait for that exact group;
    # a whole-module ("module") or whole-suite request collects every summary the
    # run emits (the VM powers itself off via rosautotest /s when finished).
    want = (cfg.test.partition(":")[2] or None) if cfg.test else None
    spec = VmSpec(name=f"rostest-rostests-{cfg.tag}", disks=disks, boot_order="c",
                  serial_files=[com1], memory_mb=cfg.memory, smp=cfg.smp,
                  kvm=cfg.kvm, display=cfg.display,
                  monitor_sock=os.path.join(cfg.workdir, "rostests-mon.sock"))

    # rosautotest may need to ride the GuiRunOnce -> Run-key reboot chain.
    deadline = time.time() + cfg.timeout
    for boot in range(1, cfg.max_boots + 1):
        _log(f"rostests boot {boot} (target group: {want or 'all'})")
        backend.launch(spec)

        def done(text):
            groups = {m.group(1) for m in SUMMARY_RE.finditer(text)}
            return bool(want and want in groups)

        outcome = _wait(backend, spec, deadline, done,
                        heartbeat=lambda _t: _log("  rosautotest running..."))
        text = _read(com1)
        backend.stop(spec)

        groups = _parse_groups(text)
        if want and want in groups:
            res.groups = [groups[want]]
            return res
        if not want and groups:
            res.groups = list(groups.values())
            return res
        if outcome == "crash":
            res.note = "crash during rostests boot"
        if time.time() >= deadline:
            break

    if not res.groups:
        res.crashed = True
        res.note = res.note or "no matching summary captured"
    return res


def _parse_groups(text: str) -> "Dict[str, GroupResult]":
    out = {}
    for m in SUMMARY_RE.finditer(text):
        out[m.group(1)] = GroupResult(
            name=m.group(1), executed=int(m.group(2)), todo=int(m.group(3)),
            failures=int(m.group(4)), skipped=int(m.group(5) or 0))
    return out
