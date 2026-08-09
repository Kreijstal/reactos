#!/usr/bin/env python3
"""Install msys2 onto a fresh NTFS ReactOS install, pulling it from the host
SMB share \\10.0.2.2\\public via the branch's smb2rdr/libsmb2 redirector, and
watch for the MM page-reuse family surfacing (bug5 family) under the load.

Phases (argv[1]):
  install  - fresh NTFS install of build_nt10/bootcd.iso -> DISK
  boot     - boot the installed HD with SLIRP net (hostfwd 7000) + COM2 + gdb,
             wait for luagent to listen on :7000
  smb      - connect luagent, smoke-test \\10.0.2.2\\public, then xcopy the
             whole msys64 tree to C:\\msys64 while watching for crashes
  all      - install, boot, smb in sequence

The installed system autostarts hello.exe (Run key ReactOSSerialShell), which
brings up the luagent RXSH agent on guest :7000 (hostfwd to host 127.0.0.1:7000).
"""
import sys, os, time, subprocess, socket
sys.path.insert(0, '/home/kreijstal/git/reactos')
from ros_test import (RosQemu, LuagentClient, monitor_copy,
                      finish_install, boot_from_hd, drive_first_boot_wizard)

ROOT = '/home/kreijstal/git/reactos'
ISO  = f'{ROOT}/build_nt10/bootcd.iso'
DISK = '/tmp/nt10msys.img'
NAME = 'nt10msys'
HOST_PORT = 7100          # avoid clashing with any stale :7000 user
COM2 = f'/tmp/{NAME}-com2.sock'
LOG  = f'/tmp/{NAME}.log'
SOCK = f'/tmp/{NAME}.sock'


def drive_text_setup(q):
    """Drive this build's near-silent usetup blind (keystrokes per bug6.md).
    bootcd.ini DefaultOS=Setup_Debug, so one ENTER at the freeldr menu boots
    setup. Then: 3xENTER through intro -> partition list; ENTER onto the
    unpartitioned space; 2xDOWN to 'NTFS (quick format)'; ENTER select; ENTER
    confirm. Anchor on the copy markers this build DOES emit."""
    print("=== text setup (blind) ===", flush=True)
    time.sleep(3)
    q.send("ret")                       # boot Setup_Debug (freeldr default)
    q.wait("SourcePath (1):", timeout=120)
    time.sleep(2)
    # Walk the intro + partition pages by pressing ENTER until the filesystem
    # format list appears (the first screen that emits SETUP_SELECTED). Anchor
    # on that marker rather than fixed sleeps — guest boot speed varies per run.
    print("  ENTER-walk intro/partition -> filesystem list", flush=True)
    def saw_selected():
        return any("SETUP_SELECTED:" in l for l in q.log_lines())
    deadline = time.time() + 90
    while not saw_selected() and time.time() < deadline:
        q.send("ret")
        t0 = time.time()
        while time.time() - t0 < 4 and not saw_selected():
            time.sleep(0.5)
    if not saw_selected():
        q.screenshot("setup_stuck_no_fslist")
        raise SystemExit("never reached filesystem format list")
    # The list re-emits SETUP_SELECTED on every scroll; step down until NTFS is
    # highlighted (robust to key timing, unlike blind sends).
    print(f"  fs list default: {q.last_selected()}", flush=True)
    for i in range(12):
        if "NTFS" in q.last_selected():
            break
        q.do("down", "SETUP_SELECTED:", 5, f"fs option {i+1}")
    else:
        raise SystemExit(f"never reached NTFS, last: {q.last_selected()}")
    print(f"  selected: {q.last_selected()}", flush=True)
    q.send("ret"); time.sleep(1.0)      # select NTFS quick
    q.send("ret"); time.sleep(1.0)      # confirm format
    q.wait("FormatState: FormatDone", timeout=120)
    print("=== NTFS format done; silent file copy in progress ===", flush=True)
    # This build emits NO serial during the file copy and NONE on the
    # bootloader-install page that follows it; both are interactive/quiet.
    # finish_install() mashes ENTER on a 2s cadence while watching for the
    # post-copy markers ("Install NTFS bootcode:" / SUCCESS / NtfsShutdown),
    # so it drives straight through the silent copy and the bootloader page.
    # Give it a generous window for the multi-minute silent copy.


def do_install():
    print("=== PHASE install ===", flush=True)
    if os.path.exists(DISK):
        os.unlink(DISK)
    subprocess.run(["truncate", "-s", "10G", DISK], check=True)
    q = RosQemu(name=NAME, iso=ISO, disk=DISK)
    q.start(boot="d")
    drive_text_setup(q)
    # finish_install ENTER-mashes through the silent copy + bootloader page to
    # a clean reboot; allow up to ~12 min for the copy on this slow NTFS path.
    finish_install(q, success_timeout=720)
    print("=== text-setup install done; driving GUI first-boot wizard ===",
          flush=True)
    # Second stage: boot HD, run the GUI wizard to reach a usable desktop.
    if not boot_from_hd(q):
        raise SystemExit("first HD boot failed")
    drive_first_boot_wizard(q)
    print("=== install complete, disk at", DISK, "===", flush=True)
    q.kill()


def boot_hd_net(mem=1024, gdb=True):
    """Boot the installed HD image with SLIRP user-net (hostfwd 7000->host),
    rtl8139 NIC (so 10.0.2.2 routes to host SMB), COM1 log, COM2 socket, gdb."""
    for pid in subprocess.run(["pgrep", "-f", f"qemu.*-name {NAME}\\b"],
                              capture_output=True, text=True).stdout.split():
        try: os.kill(int(pid), 9)
        except ProcessLookupError: pass
    time.sleep(0.5)
    for f in (LOG, SOCK, COM2):
        if os.path.exists(f):
            os.unlink(f)
    cmd = [
        "qemu-system-x86_64", "-m", str(mem),
        "-drive", f"file={DISK},format=raw,if=ide", "-boot", "c",
        "-netdev", f"user,id=net0,hostfwd=tcp:127.0.0.1:{HOST_PORT}-:7000",
        "-device", "rtl8139,netdev=net0",
        "-chardev", f"file,id=com1,path={LOG}",
        "-chardev", f"socket,id=com2,path={COM2},server=on,wait=off",
        "-serial", "chardev:com1",
        "-serial", "chardev:com2",
        "-monitor", f"unix:{SOCK},server,nowait",
        "-rtc", "base=2026-06-02T12:00:00,clock=host",
        "-display", "gtk", "-no-reboot", "-enable-kvm",
        "-name", NAME, "-daemonize",
    ]
    if gdb:
        cmd += ["-s"]
    subprocess.run(cmd, check=True, capture_output=True)
    time.sleep(0.5)
    out = subprocess.run(["pgrep", "-f", f"qemu.*-name {NAME}\\b"],
                         capture_output=True, text=True).stdout.split()
    q = RosQemu(name=NAME, iso=ISO, disk=DISK)
    q.pid = int(out[0]) if out else None
    q.com2_sock = COM2
    q.host_port = HOST_PORT
    print(f"=== PHASE boot: PID={q.pid} hostfwd 127.0.0.1:{HOST_PORT}->guest:7000 ===",
          flush=True)
    return q


LIVE_ISO = f'{ROOT}/build_nt10/liveimg.iso'


def boot_live_net(mem=2048, gdb=True):
    """Boot the livecd (liveimg.iso — bundles hello.exe/luagent, autologs in)
    with the installed NTFS image attached as a SECONDARY disk. The live system
    runs from CD/ramdisk; the ReactOS-formatted NTFS volume on the secondary
    disk is the write target for the SMB workload (exercises NTFS+Cc+MM)."""
    for pid in subprocess.run(["pgrep", "-f", f"qemu.*-name {NAME}\\b"],
                              capture_output=True, text=True).stdout.split():
        try: os.kill(int(pid), 9)
        except ProcessLookupError: pass
    time.sleep(0.5)
    for f in (LOG, SOCK, COM2):
        if os.path.exists(f):
            os.unlink(f)
    cmd = [
        "qemu-system-x86_64", "-m", str(mem),
        "-cdrom", LIVE_ISO, "-boot", "d",
        "-drive", f"file={DISK},format=raw,if=ide,index=0",
        "-netdev", f"user,id=net0,hostfwd=tcp:127.0.0.1:{HOST_PORT}-:7000",
        "-device", "rtl8139,netdev=net0",
        "-chardev", f"file,id=com1,path={LOG}",
        "-chardev", f"socket,id=com2,path={COM2},server=on,wait=off",
        "-serial", "chardev:com1",
        "-serial", "chardev:com2",
        "-monitor", f"unix:{SOCK},server,nowait",
        "-rtc", "base=2026-06-02T12:00:00,clock=host",
        "-display", "gtk", "-no-reboot", "-enable-kvm",
        "-name", NAME, "-daemonize",
    ]
    if gdb:
        cmd += ["-s"]
    subprocess.run(cmd, check=True, capture_output=True)
    time.sleep(0.5)
    out = subprocess.run(["pgrep", "-f", f"qemu.*-name {NAME}\\b"],
                         capture_output=True, text=True).stdout.split()
    q = RosQemu(name=NAME, iso=LIVE_ISO, disk=DISK)
    q.pid = int(out[0]) if out else None
    q.com2_sock = COM2
    q.host_port = HOST_PORT
    print(f"=== PHASE live: PID={q.pid} liveimg+secondary-NTFS, "
          f"hostfwd 127.0.0.1:{HOST_PORT}->guest:7000 ===", flush=True)
    return q


def do_live_probe():
    """Boot livecd + secondary NTFS, wait for luagent, then probe the drive
    layout (which letter the installed NTFS volume got, and SystemRoot)."""
    print("=== PHASE live-probe ===", flush=True)
    q = boot_live_net()
    lu = wait_luagent(q)
    run(lu, q, "cmd.exe", "cmd.exe", "/c",
        r"echo SYSROOT=%SystemRoot% & fsutil fsinfo drives & "
        r"for %d in (C D E F G) do @(echo ==DRIVE %d:== & dir %d:\ 2>nul)",
        label="probe-drives", win=120)
    print("=== live-probe done (qemu left running for follow-up) ===", flush=True)


def wait_luagent(q, timeout=300):
    """luagent's "net listening" banner prints to COM2 (where hello.exe runs),
    NOT the COM1 kernel-debug log — and a bare SLIRP host connect() succeeds
    even with no guest listener. So prove readiness by retrying a real RXSH
    HELLO handshake on the hostfwd TCP port until it ACKs (or the boot crashes)."""
    print(f"  waiting for luagent RXSH HELLO on :{HOST_PORT} (<= {timeout}s)...",
          flush=True)
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        # bail early if the guest bugchecked during boot
        if any(("Assertion" in l or "BugCheck" in l or "KDB: Break" in l)
               for l in q.log_lines()[-40:]):
            raise SystemExit("guest crashed during boot; check " + LOG)
        try:
            lu = LuagentClient(q, port=HOST_PORT, hello_timeout=4)
            print("  luagent HELLO acked — agent is up", flush=True)
            return lu
        except Exception as e:
            last = e
            time.sleep(3)
    raise SystemExit(f"luagent never answered HELLO ({last!r}); check {LOG}")


def run(lu, q, *argv, label="", win=600, idle=600):
    """spawn argv[0] with argv, stream output until exit/timeout/crash."""
    print(f"\n--- RUN {label or argv}", flush=True)
    lu.spawn(argv[0], *argv, timeout_ms=win*1000, idle_timeout_ms=idle*1000)
    res, kv = lu.stream(deadline=time.time() + win)
    print(f"--- {label} -> {res} {kv}", flush=True)
    return res, kv


def do_smb():
    print("=== PHASE smb ===", flush=True)
    q = boot_hd_net()
    lu = wait_luagent(q)
    CMD = r"C:\ReactOS\system32\cmd.exe"
    # 1) confirm the NIC/route + SMB share are reachable
    run(lu, q, CMD, CMD, "/c", r"dir \\10.0.2.2\public", label="dir-share", win=120)
    # 2) copy one small file first (smoke test smb2rdr read path)
    run(lu, q, CMD, CMD, "/c",
        r"copy /Y \\10.0.2.2\public\msys.png C:\msys.png && dir C:\msys.png",
        label="copy-one", win=120)
    # 3) the heavy workload: recursively pull the whole tree -> stresses
    #    smb2rdr read + NTFS write + Cc + MM (the bug5 family surface)
    run(lu, q, CMD, CMD, "/c",
        r"xcopy /E /I /Y /C \\10.0.2.2\public\msys64 C:\msys64",
        label="xcopy-msys64", win=5400, idle=900)
    print("=== smb workload returned without a guest crash ===", flush=True)


if __name__ == '__main__':
    phase = sys.argv[1] if len(sys.argv) > 1 else 'all'
    if phase in ('install', 'all'):
        do_install()
    if phase in ('boot',):
        q = boot_hd_net(); wait_luagent(q)
    if phase in ('smb', 'all'):
        do_smb()
    if phase in ('live-probe',):
        do_live_probe()
