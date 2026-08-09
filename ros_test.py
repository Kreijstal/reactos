#!/usr/bin/env python3
"""
ReactOS NTFS install / boot / reinstall test harness.

Replaces utils.sh with a Python library that drives QEMU via the unix monitor
socket and observes the system through the serial-port log.  Designed to be
imported and driven step-by-step from `python3 -c '...'` so you can adapt the
test to whatever the system is actually doing instead of running a fragile
"happy path" end-to-end script.

------------------------------------------------------------------
                          Workflow recipes
------------------------------------------------------------------

# Recipe 1: fresh install on a brand-new disk
    from ros_test import RosQemu, install_ntfs, monitor_copy, finish_install
    q = RosQemu("rostest", "build/bootcd.iso")
    q.start(boot="d")
    install_ntfs(q)
    monitor_copy(q)
    finish_install(q)  # advances SUCCESS->FLUSH->REBOOT and waits for clean exit

# Recipe 2: reinstall over an existing install
    q = RosQemu("rostest", "build/bootcd.iso", disk="/tmp/rostest.img")
    q.start(boot="d")
    install_ntfs(q)
    monitor_copy(q)
    finish_install(q)

# WARNING: do NOT q.kill() before finish_install returns. SUCCESS_PAGE waits
# 15s/ENTER before advancing to FLUSH_PAGE -> REBOOT_PAGE -> NtShutdownSystem.
# NTFS's IRP_MJ_SHUTDOWN handler (NtfsFlushVolume) is the only thing that
# pushes the BOOTLOADER_INSTALL writes (freeldr.ini, BOOTSECT.OLD) out of Cc.
# Killing earlier leaves those non-resident files with allocated clusters but
# zero contents on disk — the partition then drops freeldr into its
# "Setup and Configuration" menu instead of booting ReactOS.

# Recipe 3: full end-to-end (install + HD boot verification)
    python3 ros_test.py build/bootcd.iso

------------------------------------------------------------------
                          State on disk
------------------------------------------------------------------

For each instance `name`, RosQemu owns three files:
    /tmp/{name}.img    # the disk (raw or qcow2 by extension)
    /tmp/{name}.log    # QEMU's serial output (the only event source)
    /tmp/{name}.sock   # QEMU's monitor unix socket (sendkey, screendump, ...)

Display is on VNC :84 (port 5984).  No `-nic` so the guest can't escape.
"""
import socket, time, subprocess, os, sys, re, signal, struct, base64

def _qemu_pids_for_disk(disk):
    disk_name = os.path.basename(disk)
    try:
        out = subprocess.check_output(["ps", "-eo", "pid=,comm=,args="],
                                      text=True, stderr=subprocess.DEVNULL)
    except subprocess.CalledProcessError:
        return []

    pids = []
    for line in out.splitlines():
        parts = line.strip().split(None, 2)
        if len(parts) < 3:
            continue
        pid, comm, args = parts
        if comm.startswith("qemu-system-") and disk_name in args:
            pids.append(int(pid))
    return pids

class RosQemu:
    def __init__(self, name="rostest", iso="build/bootcd.iso", disk=None):
        self.name = name
        self.sock_path = f"/tmp/{name}.sock"
        self.log_path = f"/tmp/{name}.log"
        self.iso = iso
        self.pid = None

        if disk:
            self.disk = disk
        else:
            self.disk = f"/tmp/{name}.img"
            if os.path.exists(self.disk):
                os.unlink(self.disk)
            subprocess.run(["truncate", "-s", "10G", self.disk], check=True)

        self.fmt = "qcow2" if self.disk.endswith(".qcow2") else "raw"

    def start(self, boot="d"):
        """Launch QEMU.  boot="d" boots from CD, boot="c" from HD."""
        # Kill old instance
        try:
            for pid in _qemu_pids_for_disk(self.disk):
                os.kill(int(pid), 9)
            time.sleep(1)
        except ProcessLookupError:
            pass

        for f in [self.log_path, self.sock_path]:
            if os.path.exists(f):
                os.unlink(f)

        cmd = [
            "qemu-system-x86_64", "-m", "2048",
            "-drive", f"file={self.disk},format={self.fmt},if=ide",
            "-boot", boot,
            "-serial", f"file:{self.log_path}",
            "-monitor", f"unix:{self.sock_path},server,nowait",
            "-display", "none",
            "-no-reboot", "-nic", "user,model=rtl8139",
            "-enable-kvm", "-daemonize",
        ]
        if boot == "d":
            cmd += ["-cdrom", self.iso]

        subprocess.run(cmd, check=True, capture_output=True)
        time.sleep(0.5)

        pids = _qemu_pids_for_disk(self.disk)
        self.pid = pids[0] if pids else None

        print(f"QEMU started: PID={self.pid} disk={self.disk} boot={boot}")
        return self

    def start_luagent(self, usb_image, host_port=7000, mem=1024,
                      gdb=True, com2_sock=None):
        """Boot the bootcd ISO with a USB-MSC drive (so partmgr exposes it as C:),
        a hostfwd'd rtl8139 NIC for luagent (port 7000 in guest -> host_port on
        host), COM1 streamed to {name}.log, COM2 on a unix socket for the
        serial-shell (used by hello.exe), and -s for a gdb stub.
        """
        # Kill any prior instance of this name
        try:
            out = subprocess.check_output(
                ["pgrep", "-f", f"qemu.*-name {self.name}\\b"],
                text=True, stderr=subprocess.DEVNULL)
            for pid in out.strip().split():
                os.kill(int(pid), 9)
            time.sleep(0.5)
        except (subprocess.CalledProcessError, ProcessLookupError):
            pass

        if com2_sock is None:
            com2_sock = f"/tmp/{self.name}-com2.sock"
        for f in [self.log_path, self.sock_path, com2_sock]:
            if os.path.exists(f):
                os.unlink(f)
        self.com2_sock = com2_sock
        self.host_port = host_port

        cmd = [
            "qemu-system-x86_64", "-m", str(mem),
            "-cdrom", self.iso, "-boot", "d",
            "-device", "qemu-xhci,id=xhci",
            "-drive", f"if=none,id=usbdrv,file={usb_image},format=raw",
            "-device", "usb-storage,bus=xhci.0,drive=usbdrv",
            "-netdev", f"user,id=net0,hostfwd=tcp:127.0.0.1:{host_port}-:7000",
            "-device", "rtl8139,netdev=net0",
            "-chardev", f"file,id=com1,path={self.log_path}",
            "-chardev", f"socket,id=com2,path={com2_sock},server=on,wait=off",
            "-serial", "chardev:com1",
            "-serial", "chardev:com2",
            "-monitor", f"unix:{self.sock_path},server,nowait",
            "-rtc", "base=2026-05-07T12:00:00,clock=host",
            "-display", "gtk",
            "-no-reboot", "-enable-kvm",
            "-name", self.name, "-daemonize",
        ]
        if gdb:
            cmd += ["-s"]
        subprocess.run(cmd, check=True, capture_output=True)
        time.sleep(0.5)
        try:
            out = subprocess.check_output(
                ["pgrep", "-f", f"qemu.*-name {self.name}\\b"],
                text=True, stderr=subprocess.DEVNULL)
            self.pid = int(out.strip().split()[0])
        except Exception:
            self.pid = None
        print(f"QEMU started (luagent rig): PID={self.pid} usb={usb_image} "
              f"host_port={host_port}", flush=True)
        return self

    def wait_luagent_listen(self, timeout=180):
        """Block until the in-guest luagent has bound and is accepting on
        port 7000.  We match the diag_log line emitted *after* uv_listen
        succeeds (not the early `server_run` trace, which races vs. the bind)."""
        return self.wait("net    listening host=0.0.0.0 port=7000",
                         timeout=timeout)

    def luagent(self, host="127.0.0.1", port=None, hello_timeout=10):
        """Open a LuagentClient against this guest's hostfwd port."""
        if port is None:
            port = getattr(self, "host_port", 7000)
        return LuagentClient(self, host=host, port=port, hello_timeout=hello_timeout)

    def kill(self):
        if hasattr(self, '_mon') and self._mon:
            try:
                self._mon.close()
            except Exception:
                pass
            self._mon = None
        if self.pid:
            try:
                os.kill(self.pid, 9)
            except ProcessLookupError:
                pass
            self.pid = None

    def restart(self, boot="d"):
        self.kill()
        time.sleep(1)
        return self.start(boot=boot)

    def _ensure_monitor(self):
        if hasattr(self, '_mon') and self._mon:
            return
        self._mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._mon.settimeout(2)
        self._mon.connect(self.sock_path)
        try:
            self._mon.recv(4096)
        except socket.timeout:
            pass
        self._mon.settimeout(1)

    def send(self, key):
        self.monitor_cmd(f"sendkey {key}")

    def monitor_cmd(self, cmd):
        try:
            self._ensure_monitor()
            self._mon.sendall((cmd + "\n").encode())
            time.sleep(0.05)
            try:
                self._mon.recv(4096)
            except socket.timeout:
                pass
        except Exception as e:
            print(f"  monitor_cmd({cmd!r}) failed: {e}", file=sys.stderr)
            self._mon = None

    def is_alive(self):
        if self.pid:
            try:
                os.kill(self.pid, 0)
                return True
            except (ProcessLookupError, PermissionError):
                return False
        return bool(_qemu_pids_for_disk(self.disk))

    def screenshot(self, name="shot"):
        path = f"/tmp/{self.name}-{name}.ppm"
        if os.path.exists(path):
            os.unlink(path)
        self.monitor_cmd(f"screendump {path}")
        deadline = time.time() + 2
        last = -1
        while time.time() < deadline:
            if os.path.exists(path):
                sz = os.path.getsize(path)
                if sz > 0 and sz == last:
                    break
                last = sz
            time.sleep(0.05)
        try:
            return path, open(path, "rb").read()
        except FileNotFoundError:
            return path, b""

    def log_lines(self):
        try:
            with open(self.log_path, "r", errors="replace") as f:
                return f.readlines()
        except FileNotFoundError:
            return []

    def log_count(self, pattern):
        return sum(1 for l in self.log_lines() if pattern in l)

    def spam_keys_until(self, key, pattern, timeout=30, interval=0.4):
        deadline = time.time() + timeout
        sent = 0
        while time.time() < deadline:
            if any(pattern in l for l in self.log_lines()):
                return True, sent
            self.send(key)
            sent += 1
            time.sleep(interval)
        return any(pattern in l for l in self.log_lines()), sent

    # ── Core poll loop ──

    def _poll(self, check, timeout, desc, interval=0.3, allow_crashed=False):
        """Core poll loop: call check() until truthy, bail on QEMU exit, ROS
        crash (kdb/Assertion/BugCheck), or timeout.  Set allow_crashed=True if
        the caller is intentionally polling FOR a crash."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            result = check()
            if result:
                return result
            if not self.is_alive():
                raise RuntimeError(f"QEMU exited while waiting for: {desc}")
            if not allow_crashed:
                kind = self.crashed()
                if kind:
                    raise RuntimeError(
                        f"ROS {kind} while waiting for: {desc} — last lines:\n"
                        + "".join(self.log_lines()[-10:]))
            time.sleep(interval)
        raise TimeoutError(f"timeout({timeout}s) waiting for: {desc}")

    def wait(self, pattern, timeout=10):
        def check():
            return any(pattern in l for l in self.log_lines())
        return self._poll(check, timeout, pattern)

    def wait_new(self, pattern, timeout=10):
        before = self.log_count(pattern)
        def check():
            return self.log_count(pattern) > before
        return self._poll(check, timeout, f"new: {pattern}")

    def wait_any(self, patterns, timeout=10):
        def check():
            lines = self.log_lines()
            for p in patterns:
                if any(p in l for l in lines):
                    return p
        return self._poll(check, timeout, f"any of: {patterns}", interval=0.2)

    def wait_any_new(self, patterns, timeout=10):
        before = {p: self.log_count(p) for p in patterns}
        def check():
            for p in patterns:
                if self.log_count(p) > before[p]:
                    return p
        return self._poll(check, timeout, f"new of any: {patterns}", interval=0.2)

    def do(self, key, pattern, timeout=10, label=""):
        if label:
            print(f"  {label}", file=sys.stderr)
        before = self.log_count(pattern)
        self.send(key)
        def check():
            return self.log_count(pattern) > before
        return self._poll(check, timeout, f"new: {pattern} (after '{key}')")

    # Markers that indicate the guest stopped making forward progress.
    # Order matters: more specific first.
    _CRASH_MARKERS = (
        ("assertion",     "*** Assertion failed"),
        ("bugcheck",      "*** STOP:"),
        ("bugcheck",      "*** Fatal System Error:"),
        ("bugcheck",      "BugCheck"),
        ("kdb",           "kdb:>"),
        ("luagent_crash", "luagent: unhandled exception"),
    )

    def crashed(self):
        """Return a short tag (str) describing the crash, or None."""
        lines = self.log_lines()
        for tag, marker in self._CRASH_MARKERS:
            if any(marker in l for l in lines):
                return tag
        return None

    def crash_context(self, n=15):
        """Last n log lines, joined; useful in error messages."""
        return "".join(self.log_lines()[-n:])

    def last_selected(self):
        for l in reversed(self.log_lines()):
            if "SETUP_SELECTED:" in l:
                return l.strip()
        return ""


# ──────────────────────────────────────────────────────────────────────────
#   Luagent protocol client (paired with RosQemu.start_luagent / .luagent())
# ──────────────────────────────────────────────────────────────────────────
class LuagentClient:
    """Thin client for reactos-luagent's framed protocol.

    Bound to a RosQemu instance so every blocking read also polls
    `q.crashed()`/`q.is_alive()` — no wait will sit through a kdb prompt.

    Frame layout: 'RXSH' magic (4) | type (1) | flags (1) | req_id u32 | len u32 | payload
    """
    MAGIC  = b"RXSH"
    HELLO  = 1; ACK    = 2; ERROR = 3
    OP     = 10; OPRES = 11
    SPAWN  = 40; STDOUT = 42; STDERR = 43; EXIT = 44; KILL = 45
    PING   = 50; PONG   = 51

    def __init__(self, q, host="127.0.0.1", port=7000, hello_timeout=10):
        self.q = q
        self.s = socket.create_connection((host, port), timeout=hello_timeout)
        self.s.settimeout(0.5)  # short slice; outer loops re-check q.crashed()
        # Bytes pulled off the socket but not yet consumed as a whole frame.
        # Callers treat TimeoutError as "nothing yet, try again", so a read
        # that times out PART WAY THROUGH a frame must not throw those bytes
        # away -- the retry would then resync onto the middle of the payload
        # and report a bogus "bad magic".  Everything received lands here and
        # is only removed once a complete frame has been parsed out of it.
        self._buf = b""
        self._send(self.HELLO, 1, b"client=ros_test\nversion=1")
        typ, _, _, _ = self._recv_frame(timeout=hello_timeout)
        if typ != self.ACK:
            raise RuntimeError(f"HELLO did not get ACK (got type={typ})")

    @classmethod
    def _encode(cls, t, req, payload=b""):
        return cls.MAGIC + bytes([t, 0]) + struct.pack("<II", req, len(payload)) + payload

    def _send(self, t, req, payload=b""):
        self.s.sendall(self._encode(t, req, payload))

    def _fill(self, n, deadline):
        """Ensure self._buf holds at least n bytes.  On timeout the buffer
        keeps whatever arrived, so a later retry resumes mid-frame instead of
        losing sync."""
        while len(self._buf) < n:
            try:
                c = self.s.recv(65536)
            except socket.timeout:
                if time.time() > deadline:
                    raise TimeoutError(f"want {n}, got {len(self._buf)}")
                k = self.q.crashed()
                if k:
                    raise RuntimeError(
                        f"ROS {k} during recv — last lines:\n{self.q.crash_context()}")
                if not self.q.is_alive():
                    raise RuntimeError("QEMU exited during recv")
                continue
            if not c:
                raise EOFError(f"EOF after {len(self._buf)}/{n}")
            self._buf += c

    def _recv_frame(self, timeout=30):
        deadline = time.time() + timeout
        self._fill(14, deadline)
        if self._buf[:4] != self.MAGIC:
            raise RuntimeError(f"bad magic {self._buf[:4]!r}")
        typ = self._buf[4]
        req, plen = struct.unpack("<II", self._buf[6:14])
        self._fill(14 + plen, deadline)
        payload = bytes(self._buf[14:14 + plen])
        self._buf = self._buf[14 + plen:]
        return typ, req, plen, payload

    def spawn(self, path, *argv, timeout_ms=1800000, idle_timeout_ms=600000, req=1000):
        """Send a SPAWN frame.  Note: `argv` should be the FULL argv (argv0,
        argv1, ...) matching pacman's expectations; if you want pacman to see
        argv[0] as its own path, pass `path` again as the first item."""
        if not argv:
            argv = (path,)

        def _enc_arg(a):
            # The wire format is newline-delimited key=value, so an argument
            # containing a newline would be truncated at its first '\n' by the
            # server's kv_dup.  Send such values (and any value that would be
            # mistaken for the sentinel) base64-encoded behind a "b64:" prefix;
            # the server transparently decodes it.  Plain arguments -- including
            # Windows paths full of backslashes -- go through verbatim.
            if ("\n" in a) or ("\r" in a) or a.startswith("b64:"):
                return "b64:" + base64.b64encode(a.encode()).decode("ascii")
            return a

        kv = (f"path={path}\n"
              + "\n".join(f"argv{i}={_enc_arg(a)}" for i, a in enumerate(argv))
              + f"\ntimeout_ms={timeout_ms}\nidle_timeout_ms={idle_timeout_ms}\n")
        self._send(self.SPAWN, req, kv.encode())

    def stream(self, *, on_stdout=None, on_stderr=None, deadline=None,
               echo=True):
        """Read frames forever (handling PING with auto-PONG) until either:
          • EXIT frame arrives → returns ("exit", parsed-kv-dict)
          • absolute `deadline` (time.time() epoch) elapses → ("timeout", None)
          • EOF / connection reset → ("eof", None)
          • ROS crashes / QEMU dies → raises RuntimeError via _recv_*

        If `echo` is true (default), STDOUT/STDERR payloads are mirrored to
        sys.stdout with OUT|/ERR| prefixes.  `on_stdout`/`on_stderr` get the
        raw bytes for additional handling.
        """
        proc_id = None
        while True:
            now = time.time()
            slice_to = (deadline - now) if deadline else 5.0
            if slice_to <= 0:
                return "timeout", None
            # Check the guest log every iteration so a crash that happens
            # *between* frames is surfaced immediately and not as a vague EOF.
            kind = self.q.crashed()
            if kind:
                raise RuntimeError(
                    f"ROS {kind} during stream — last lines:\n{self.q.crash_context()}")
            try:
                typ, req, plen, payload = self._recv_frame(timeout=min(slice_to, 5.0))
            except TimeoutError:
                continue
            except (EOFError, ConnectionError):
                # If the guest crashed, prefer the precise diagnosis.
                kind = self.q.crashed()
                if kind:
                    raise RuntimeError(
                        f"ROS {kind} during stream — last lines:\n{self.q.crash_context()}")
                return "eof", None

            if typ == self.PING:
                try:
                    self._send(self.PONG, req, b"status=ok")
                except Exception:
                    pass
                continue
            if typ == self.STDOUT:
                if echo:
                    sys.stdout.write("OUT|" + payload.decode(errors="replace"))
                    sys.stdout.flush()
                if on_stdout:
                    on_stdout(payload)
                continue
            if typ == self.STDERR:
                if echo:
                    sys.stdout.write("ERR|" + payload.decode(errors="replace"))
                    sys.stdout.flush()
                if on_stderr:
                    on_stderr(payload)
                continue
            if typ == self.ACK:
                kv = self._kv(payload)
                proc_id = kv.get("proc_id", proc_id)
                if echo:
                    sys.stdout.write(f"ACK|{kv}\n"); sys.stdout.flush()
                continue
            if typ == self.ERROR:
                kv = self._kv(payload)
                if echo:
                    sys.stdout.write(f"ERR-FRAME|{kv}\n"); sys.stdout.flush()
                continue
            if typ == self.EXIT:
                kv = self._kv(payload)
                if echo:
                    sys.stdout.write(f"EXIT|{kv}\n"); sys.stdout.flush()
                return "exit", kv
            if echo:
                sys.stdout.write(f"UNK|type={typ} req={req} len={plen}\n")
                sys.stdout.flush()

    @staticmethod
    def _kv(payload):
        try:
            text = payload.decode(errors="replace")
        except Exception:
            return {"_raw": repr(payload)}
        out = {}
        for line in text.splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                out[k] = v
        return out

    def close(self):
        try: self.s.close()
        except Exception: pass


def install_ntfs(q):
    """Drive usetup from boot prompt to FILE_COPY."""
    print("=== NTFS Install ===")

    # The NT10/amd64 bootcd used here does not emit the older SETUP_PAGE
    # breadcrumbs. Drive the visible text setup path and anchor only on the
    # serial lines this build still emits.
    print("  boot menu -> setup debug")
    q.send("home")
    time.sleep(0.2)
    for _ in range(3):
        q.send("down")
        time.sleep(0.1)
    q.send("ret")
    q.wait("SourcePath (1):", timeout=90)
    time.sleep(2)

    print("  setup intro -> partition list")
    for _ in range(3):
        q.send("ret")
        time.sleep(0.8)

    print("  create/use primary partition -> filesystem list")
    q.send("ret")
    try:
        q.wait("SETUP_SELECTED:", timeout=30)
    except TimeoutError:
        print("  existing install page detected -> clean install path")
        q.send("esc")
        time.sleep(1)
        q.send("ret")
        time.sleep(1)
        q.send("ret")
        q.wait("SETUP_SELECTED:", timeout=30)

    for i in range(12):
        q.do("down", "SETUP_SELECTED:", 5, f"filesystem option {i + 1}")
        sel = q.last_selected()
        if "NTFS" in sel:
            break
    else:
        raise RuntimeError(f"Expected NTFS, got: {q.last_selected()}")
    print(f"  Confirmed: {sel}")

    print("  accepting NTFS quick format")
    q.send("ret")
    time.sleep(1)
    q.send("ret")
    phase = q.wait_any_new([
        "NtfsChkdsk at",
        "NtOpenFile failed: c0000034",
        "Copy hive:",
        "Install NTFS bootcode:",
    ], timeout=120)

    if phase == "NtfsChkdsk at":
        print("  install directory -> file copy")
        q.send("ret")
        q.wait_any_new([
            "NtOpenFile failed: c0000034",
            "Copy hive:",
            "Install NTFS bootcode:",
        ], timeout=120)
    print("=== FILE_COPY started ===")


def monitor_copy(q, timeout_per_pct=180):
    """Watch FILE_COPY, bail on crash/OOM/stall."""
    print("=== Monitoring file copy ===")
    last_pct = ""
    last_time = time.time()
    start = time.time()

    while True:
        if not q.is_alive():
            print(f"QEMU exited after {time.time()-start:.0f}s")
            return False

        if q.crashed():
            print(f"CRASH after {time.time()-start:.0f}s!")
            for l in q.log_lines():
                if "Assertion" in l or "kdb:>" in l:
                    print(f"  {l.strip()}")
            return False

        lines = q.log_lines()
        pcts = [l for l in lines if "PROGRESS:" in l]
        if pcts:
            m = re.search(r'(\d+)%', pcts[-1])
            if m and m.group(0) != last_pct:
                last_pct = m.group(0)
                last_time = time.time()
                print(f"  Progress: {last_pct} ({time.time()-start:.0f}s)")

        if any("SETUP_PAGE:REGISTRY" in l or "SETUP_PAGE:BOOT_LOADER_INSTALL" in l
               or "SETUP_PAGE:SUCCESS" in l or "Install NTFS bootcode:" in l
               or "NtfsShutdown()" in l for l in lines):
            print(f"FILE_COPY complete ({time.time()-start:.0f}s)")
            return True

        if time.time() - last_time > timeout_per_pct:
            print(f"STALL: no progress for {timeout_per_pct}s (last: {last_pct})")
            return False

        time.sleep(1)


def finish_install(q, success_timeout=120, reboot_timeout=120):
    """Walk usetup from monitor_copy's exit point to a clean guest reboot.

    Why this exists: SuccessPage(usetup.c) waits 15s or for ENTER, then
    falls through FlushPage -> REBOOT_PAGE -> NtShutdownSystem(ShutdownReboot).
    The IRP_MJ_SHUTDOWN that fires next is the ONLY pass that runs
    NtfsFlushVolume / CcFlushCache on the BOOTLOADER_INSTALL writes
    (freeldr.ini, BOOTSECT.OLD, etc) so they actually reach disk. If the
    caller q.kill()s before NtShutdownSystem runs, those non-resident
    files end up with allocated clusters but zero contents — freeldr
    then has nothing to load and drops into its setup-and-configuration
    menu on next boot.

    QEMU is launched with -no-reboot, so it terminates when the guest
    requests a reboot. We just wait for that exit; that's the signal
    that the shutdown handler ran to completion.
    """
    print("=== Driving usetup to clean reboot ===")

    # 1) Advance through any post-copy pages until either the old explicit
    #    SETUP_PAGE:SUCCESS marker appears, or the current build reaches the
    #    NTFS bootcode/shutdown path.
    deadline = time.time() + success_timeout
    while time.time() < deadline:
        lines = q.log_lines()
        if any("SETUP_PAGE:SUCCESS" in l or "Install NTFS bootcode:" in l
               or "NtfsShutdown()" in l for l in lines[-300:]):
            print("  setup success/shutdown path reached")
            break
        if not q.is_alive():
            print("  QEMU exited during setup finalization")
            return True
        q.send("ret")
        time.sleep(2)
    else:
        raise TimeoutError(
            f"never saw SETUP_PAGE:SUCCESS in {success_timeout}s")

    # 2) Send the final ENTER. SuccessPage returns FLUSH_PAGE; FlushPage
    #    returns REBOOT_PAGE; the main loop falls out; FinishSetup runs;
    #    NtShutdownSystem(ShutdownReboot) fires; IRP_MJ_SHUTDOWN walks the
    #    NTFS VCB list and flushes every dirty FCB; guest reboots; QEMU
    #    -no-reboot makes the process exit.
    print("  Waiting for FLUSH->REBOOT path")

    # 3) Wait for QEMU to exit on the guest's reboot request.
    deadline = time.time() + reboot_timeout
    while time.time() < deadline:
        if not q.is_alive():
            print("  QEMU exited cleanly — NtShutdownSystem flush completed")
            return True
        q.send("ret")
        time.sleep(1)

    raise TimeoutError(
        f"QEMU did not exit within {reboot_timeout}s after SUCCESS; "
        f"the shutdown path probably hung. Do NOT kill blindly — first "
        f"investigate, killing now will lose dirty Cc data.")


def boot_from_hd(q):
    """Kill install QEMU, relaunch with boot=c, wait for session init."""
    print("=== Booting from HD ===")
    q.kill()
    time.sleep(1)

    for f in [q.log_path, q.sock_path]:
        if os.path.exists(f):
            os.unlink(f)

    cmd = [
        "qemu-system-x86_64", "-m", "512",
        "-drive", f"file={q.disk},format={q.fmt},if=ide",
        "-boot", "c",
        "-serial", f"file:{q.log_path}",
        "-monitor", f"unix:{q.sock_path},server,nowait",
        "-display", "gtk",
        "-no-reboot", "-nic", "none", "-enable-kvm", "-daemonize",
    ]
    subprocess.run(cmd, check=True, capture_output=True)
    time.sleep(0.5)

    try:
        pids = _qemu_pids_for_disk(q.disk)
        q.pid = pids[0] if pids else None
    except Exception:
        q.pid = None

    print(f"  Booting from HD (PID={q.pid})")

    deadline = time.time() + 120
    while time.time() < deadline:
        if q.crashed():
            print("FAIL: BSOD/assertion on HD boot!")
            for l in q.log_lines():
                if "Assertion" in l or "kdb:>" in l:
                    print(f"  {l.strip()}")
            return False

        if not q.is_alive():
            print("FAIL: QEMU exited unexpectedly")
            return False

        if any("Session 0 is ready" in l for l in q.log_lines()):
            print("PASS: Booted to session init!")
            return True

        time.sleep(1)

    print("TIMEOUT: no crash, no session after 120s")
    for l in q.log_lines()[-10:]:
        print(f"  {l.strip()}")
    return False


def drive_first_boot_wizard(q, max_pages=12, page_timeout=45):
    """Drive the GUI first-boot wizard.

    Text-mode setup is observable through serial, but this GUI wizard is not.
    Treat each ENTER as a single "Next" action, then wait for either a screen
    transition, a guest reboot, or a crash. Some pages are async workers
    ("Installing devices", "Network Install"), so later steps intentionally
    use longer timeouts and do not treat a temporarily unchanged framebuffer as
    success.
    """
    import hashlib

    def screen_hash(name):
        _, data = q.screenshot(name)
        return hashlib.md5(data).hexdigest() if data else None

    def wait_screen_change(old_hash, timeout, label):
        start = time.time()
        last_report = start
        while time.time() - start < timeout:
            if not q.is_alive():
                print(f"  {label}: guest rebooted/exited")
                return "exit"
            if q.crashed():
                print(f"  {label}: crash detected")
                return "crash"

            new_hash = screen_hash(label.replace(" ", "_"))
            if new_hash and old_hash and new_hash != old_hash:
                print(f"  {label}: advanced after {time.time() - start:.1f}s")
                return "changed"

            if time.time() - last_report >= 30:
                print(f"  {label}: still waiting ({time.time() - start:.0f}s)")
                last_report = time.time()
            time.sleep(1)

        print(f"  {label}: no framebuffer change after {timeout}s")
        return "timeout"

    print("=== Driving first-boot wizard ===")
    step_timeouts = [
        page_timeout, page_timeout, page_timeout, page_timeout,
        240, 240, 180, 120, 120, 120, 120, 120,
    ]

    for i in range(max_pages):
        if not q.is_alive():
            print(f"  guest exited before wizard step {i}")
            return True
        if q.crashed():
            print(f"  CRASH detected at wizard step {i}")
            return False

        cur_hash = screen_hash(f"wiz{i}")
        if cur_hash is None:
            print(f"  step {i}: screenshot failed, retrying")
            time.sleep(0.5)
            continue

        timeout = step_timeouts[i] if i < len(step_timeouts) else page_timeout
        print(f"  step {i}: {cur_hash[:8]} -> ENTER (timeout {timeout}s)")
        q.send("ret")
        result = wait_screen_change(cur_hash, timeout, f"wizard step {i}")
        if result == "exit":
            return True
        if result == "crash":
            return False

    # Wait for reboot, but detect stalls: if the screen doesn't change
    # for stall_timeout seconds, send Enter (an error dialog may be
    # blocking) and resume waiting.
    print("=== Waiting for guest reboot (or crash) ===")
    stall_timeout = 90
    last_change = time.time()
    prev_hash = None
    deadline = time.time() + 600
    while time.time() < deadline:
        if not q.is_alive():
            elapsed = int(time.time() - (deadline - 600))
            print(f"  guest rebooted (qemu exited) at t={elapsed}s")
            return True
        if q.crashed():
            print("  CRASH/kdb during finalize phase")
            return False

        _, data = q.screenshot("finalize")
        h = hashlib.md5(data).hexdigest() if data else None
        if h and h != prev_hash:
            prev_hash = h
            last_change = time.time()
        elif time.time() - last_change > stall_timeout:
            print(f"  stall detected ({stall_timeout}s unchanged), sending Enter")
            q.send("ret")
            last_change = time.time()

        time.sleep(2)

    print("  timeout: wizard did not finalize within 600s")
    return False
