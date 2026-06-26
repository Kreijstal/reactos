#!/usr/bin/env python3
#
# PROJECT:     ReactOS host-side test runner
# LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
# PURPOSE:     Pluggable virtual-machine backends for the rostest runner.
#
# A backend knows how to boot a VM from a VmSpec (a backend-agnostic description
# of disks, an optional CD, serial output files and a few knobs), tell whether
# it is still running, and stop it.  The suite logic (suites.py) never touches a
# concrete emulator -- it only ever talks to a VmBackend, so adding a new
# emulator is a matter of implementing this interface.
#
# QEMU is the reference backend and is exercised end-to-end.  VirtualBox is a
# second, fully wired backend; it is built from the documented VBoxManage
# command surface but is not part of the verified path on hosts without
# VirtualBox installed.

import os
import abc
import time
import subprocess
from dataclasses import dataclass, field
from typing import List, Optional, Tuple


@dataclass
class VmSpec:
    """Backend-agnostic description of a VM to boot."""
    name: str                                   # unique label (process/VM name)
    # Each disk is (path, format, interface). format is 'qcow2'/'raw'/'vdi';
    # interface is 'ide'/'sata'/'usb' (backends map it to their own flags).
    disks: List[Tuple[str, str, str]] = field(default_factory=list)
    cdrom: Optional[str] = None                 # path to an ISO, or None
    boot_order: str = "c"                       # 'c' = first disk, 'd' = CD
    serial_files: List[str] = field(default_factory=list)  # COM1, COM2, ...
    memory_mb: int = 2048
    smp: int = 1
    kvm: bool = True                            # hardware acceleration if available
    display: str = "none"                       # 'none' or 'gtk'
    netdev: bool = True                         # attach a user-mode NIC
    debug_exit_port: Optional[int] = None       # isa-debug-exit iobase (kmtest)
    monitor_sock: Optional[str] = None          # control socket path, if any


class VmBackend(abc.ABC):
    """Interface every emulator backend implements."""

    name = "abstract"

    def __init__(self):
        # Specs that have been launched and not yet stopped.  Lets the runner
        # tear every VM down on an interrupt so a daemonized guest never
        # outlives the tool and keeps a disk-image write lock.
        self._live_specs = {}

    def _mark_live(self, spec: VmSpec) -> None:
        self._live_specs[spec.name] = spec

    def _mark_stopped(self, spec: VmSpec) -> None:
        self._live_specs.pop(spec.name, None)

    def stop_all(self) -> None:
        """Stop every still-running VM this backend launched (best effort)."""
        for spec in list(self._live_specs.values()):
            try:
                self.stop(spec)
            except Exception:
                pass

    @abc.abstractmethod
    def launch(self, spec: VmSpec) -> None:
        """Start the VM described by spec (non-blocking)."""

    @abc.abstractmethod
    def is_alive(self, spec: VmSpec) -> bool:
        """True while the VM is still running."""

    @abc.abstractmethod
    def stop(self, spec: VmSpec) -> None:
        """Force the VM off (idempotent)."""

    @staticmethod
    def create(backend: str) -> "VmBackend":
        backend = (backend or "qemu").lower()
        if backend in ("qemu", "qemu-system-x86_64"):
            return QemuBackend()
        if backend in ("virtualbox", "vbox"):
            return VBoxBackend()
        raise ValueError(f"unknown VM backend: {backend!r}")


class QemuBackend(VmBackend):
    """QEMU (qemu-system-x86_64) backend -- the reference implementation."""

    name = "qemu"

    def __init__(self, qemu: Optional[str] = None, qemu_io: Optional[str] = None):
        super().__init__()
        self.qemu = qemu or os.environ.get("ROSTEST_QEMU") or "qemu-system-x86_64"
        self.qemu_io = qemu_io or os.environ.get("ROSTEST_QEMU_IO") or "qemu-io"

    # A pattern that matches only the running emulator -- never the shell that
    # launched it -- so stop()/is_alive() cannot accidentally kill ourselves.
    def _pgrep_pattern(self, spec: VmSpec) -> str:
        return rf"qemu-system.*-name {spec.name}\b"

    def _iface_flag(self, iface: str) -> str:
        # QEMU 'if=' value; usb disks need a separate device, handled below.
        return {"ide": "ide", "sata": "ide", "usb": "none"}.get(iface, "ide")

    def launch(self, spec: VmSpec) -> None:
        self.stop(spec)
        for f in list(spec.serial_files) + ([spec.monitor_sock] if spec.monitor_sock else []):
            if f and os.path.exists(f):
                try:
                    os.unlink(f)
                except OSError:
                    pass

        cmd = [self.qemu, "-m", str(spec.memory_mb), "-smp", str(spec.smp)]
        if spec.kvm:
            cmd += ["-enable-kvm"]
        for (path, fmt, iface) in spec.disks:
            cmd += ["-drive", f"file={path},format={fmt},if={self._iface_flag(iface)}"]
        if spec.cdrom:
            cmd += ["-cdrom", spec.cdrom]
        cmd += ["-boot", spec.boot_order]
        for ser in spec.serial_files:
            cmd += ["-serial", f"file:{ser}"]
        if spec.monitor_sock:
            cmd += ["-monitor", f"unix:{spec.monitor_sock},server,nowait"]
        if spec.debug_exit_port is not None:
            cmd += ["-device", f"isa-debug-exit,iobase={hex(spec.debug_exit_port)},iosize=0x01"]
        if spec.netdev:
            cmd += ["-netdev", "user,id=n0", "-device", "rtl8139,netdev=n0"]
        cmd += ["-display", spec.display, "-no-reboot", "-name", spec.name, "-daemonize"]

        subprocess.run(cmd, check=True, capture_output=True)
        self._mark_live(spec)
        time.sleep(1)

    def is_alive(self, spec: VmSpec) -> bool:
        r = subprocess.run(["pgrep", "-f", self._pgrep_pattern(spec)],
                           capture_output=True, text=True)
        return bool(r.stdout.strip())

    def stop(self, spec: VmSpec) -> None:
        subprocess.run(["pkill", "-9", "-f", self._pgrep_pattern(spec)],
                       capture_output=True)
        self._mark_stopped(spec)
        time.sleep(1)

    def zero_boot_signature(self, disk_path: str) -> None:
        """Wipe the 55AA MBR boot signature so SeaBIOS boots the install CD and
        usetup performs a clean format-install (the '--fresh' lever)."""
        subprocess.run([self.qemu_io, "-c", "write -P 0 510 2", disk_path],
                       check=True, capture_output=True)


class VBoxBackend(VmBackend):
    """VirtualBox (VBoxManage) backend.

    Wired from the documented VBoxManage surface.  NOTE: this path is not part
    of the verified test run on hosts without VirtualBox -- it is provided so
    the runner is genuinely backend-pluggable, and should be smoke-tested on a
    host that has VirtualBox before being relied upon.
    """

    name = "virtualbox"

    def __init__(self, vboxmanage: Optional[str] = None):
        super().__init__()
        self.vbm = vboxmanage or os.environ.get("ROSTEST_VBOXMANAGE") or "VBoxManage"

    def _run(self, *args, check=True):
        return subprocess.run([self.vbm, *args], check=check, capture_output=True, text=True)

    def launch(self, spec: VmSpec) -> None:
        self.stop(spec)
        # (Re)create a throwaway VM definition for this run.
        self._run("createvm", "--name", spec.name, "--ostype", "WindowsXP_64",
                  "--register", check=False)
        self._run("modifyvm", spec.name, "--memory", str(spec.memory_mb),
                  "--cpus", str(spec.smp), "--nic1", "nat", "--nictype1", "Am79C973",
                  "--boot1", "disk" if spec.boot_order == "c" else "dvd")
        # Serial port 1 -> COM1 file (raw uart capture, like QEMU -serial file:).
        if spec.serial_files:
            self._run("modifyvm", spec.name, "--uart1", "0x3F8", "4",
                      "--uartmode1", "file", spec.serial_files[0])
        self._run("storagectl", spec.name, "--name", "ide", "--add", "ide", check=False)
        port = 0
        for (path, _fmt, _iface) in spec.disks:
            self._run("storageattach", spec.name, "--storagectl", "ide",
                      "--port", str(port), "--device", "0", "--type", "hdd",
                      "--medium", path)
            port += 1
        if spec.cdrom:
            self._run("storageattach", spec.name, "--storagectl", "ide",
                      "--port", str(port), "--device", "0", "--type", "dvddrive",
                      "--medium", spec.cdrom)
        self._run("startvm", spec.name, "--type", "headless")
        self._mark_live(spec)
        time.sleep(1)

    def is_alive(self, spec: VmSpec) -> bool:
        r = self._run("list", "runningvms", check=False)
        return f'"{spec.name}"' in (r.stdout or "")

    def stop(self, spec: VmSpec) -> None:
        self._run("controlvm", spec.name, "poweroff", check=False)
        self._run("unregistervm", spec.name, "--delete", check=False)
        self._mark_stopped(spec)

    def zero_boot_signature(self, disk_path: str) -> None:
        raise NotImplementedError(
            "VBox --fresh install is not wired; provide a pre-installed VDI or use the QEMU backend.")
