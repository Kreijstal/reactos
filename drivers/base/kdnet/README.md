# KDNET Ethernet transport

This boot-time AMD64 transport uses the RTL8168/8111 PCI Ethernet controller
or the Intel 82540EM (`e1000`) without NDIS. FreeLoader selects it as
`kdnet.dll` with `DEBUGPORT=NET`. The e1000 backend provides a reproducible
QEMU test target; RTL8168 remains the hardware target for the ASUS X550DP.

Build ReactOS with the WinKD debugger core. The built-in KDBG core consumes
state-change packets locally and cannot drive a remote KDNET session:

```text
cmake -S . -B build-kdnet -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=toolchain-gcc.cmake \
  -DARCH=amd64 -DCMAKE_BUILD_TYPE=Debug \
  -D_WINKD_=TRUE -DKDBG=FALSE
cmake --build build-kdnet --target kdnet ntoskrnl
```

## Combined KDBG and WinKD boot CD

`KDBG` and `_WINKD_` may both be enabled in one AMD64 CMake configuration.
This does not put the two debugger cores in the same kernel image. Instead,
CMake builds and packages two kernels in one boot CD:

- `ntkrnlmp.exe` contains the WinKD core used by serial WinKD and KDNET;
- `ntkrnlk.exe` contains the integrated KDBG core used by screen and file
  logging.

Configure and build the combined NT 10 boot CD with:

```text
cmake -S . -B build-nt10-debug -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=toolchain-gcc.cmake \
  -DARCH=amd64 -DCMAKE_BUILD_TYPE=Debug \
  -DREACTOS_TARGET_NT=0x0A00 \
  -DKDBG=TRUE -D_WINKD_=TRUE
cmake --build build-nt10-debug --target bootcd
```

The resulting `build-nt10-debug/bootcd.iso` contains both kernels and
`kdnet.dll`. Its generated `freeldr.ini` provides local KDBG screen and file
entries that select `ntkrnlk.exe`, plus hardware and QEMU KDNET entries that
use the default WinKD kernel. No additional "dual debug" CMake option is
required.

The hardware KDNET entry can be customized at configure time:

```text
-DKDNET_BOOT_HOST_IP=192.168.250.1
-DKDNET_BOOT_TARGET_IP=192.168.250.2
-DKDNET_BOOT_PORT=50000
-DKDNET_BOOT_KEY=1.2.3.4
```

These values affect the generated boot menu; they do not add DHCP. KDNET still
uses the configured static addresses.

The boot-CD packaging rules also put `kdnet.dll` in the AMD64 setup source and
list it for installation into `system32`.

The initial implementation deliberately uses static IPv4 and polling. A
typical direct-link configuration is:

```text
/DEBUG /DEBUGPORT=NET /HOSTIP=192.168.250.1 /TARGETIP=192.168.250.2 /PORT=50000 /KEY=1.2.3.4
```

Configure the debugger host's Ethernet interface as `192.168.250.1/24` and
start the KDNET consumer before booting the target. For Radare2:

```text
r2 -d winkd://192.168.250.2:50000:1.2.3.4
```

The target resolves the host MAC with ARP. `HOSTMAC=xx-xx-xx-xx-xx-xx` can be
supplied to bypass ARP.

### Buffered DbgPrint mode

The default hardware KDNET entries add `/KDBUFFERED`. In this mode, ordinary
`DbgPrint` output is appended to the kernel's standard 32 KiB circular buffer
without sending a synchronous KD debug-I/O packet for every call. KDNET stays
fully attached: exceptions, assertions, prompts, break-ins, register and memory
requests, and state changes continue to work normally.

The buffered hardware entries retain `/SOS`, so driver-loading progress remains
visible on the target screen while ordinary `DbgPrint` traffic is collected in
the kernel buffer. Do not combine `/SOS` with `/NOGUIBOOT`: ReactOS disables
boot-video display strings when `/NOGUIBOOT` is present, which defeats the
on-screen progress needed to distinguish a slow boot from a hard hang. The
file-log entry remains headless because it is intended for unattended capture.

At every debugger stop, `scripts/roskd_cli.py` saves both the chronological log
as `dbgprint.log` and the underlying ring as `dbgprint-ring.bin` in the harvest
directory. The interactive `dmesg [PATH]` command can display or save the same
buffer. Select the explicit `hardware Ethernet KDNET (verbose harvest)` entry
when live, line-by-line `DbgPrint` streaming is required instead.

The transport mirrors its initialization and protocol diagnostics to the boot
screen and to plaintext UDP port 50001. This diagnostic stream is independent
of KDNET authentication and the inner KD session, so handshake failures remain
observable. It intentionally contains status and packet metadata, but never the
pre-shared key or derived session keys. A host can capture both channels with:

```text
tcpdump -ni <interface> -A 'udp port 50001'
tcpdump -ni <interface> -w kdnet.pcap 'udp port 50000 or udp port 50001 or arp'
```

## Early-boot harvesting

Add `/KDNET-HARVEST=ALL` to a NET-debug boot entry to enable bounded,
read-only diagnostic snapshots. The generated dual-debug boot menu enables it
for the explicitly verbose hardware entry and the QEMU KDNET entry; the default
hardware and one-CPU entries keep automatic harvesting disabled. Automatic
snapshots cover the loader memory map and loaded images during phase 0, then the
selected debug NIC and the reachable PCI topology during phase 1. PCI records
include class codes, BARs, interrupt routing, bridge topology, and enough
information to identify UHCI/OHCI/EHCI/xHCI controllers before the PnP manager
starts them.

The same option enables a plaintext, read-only request channel on UDP 50001.
It deliberately does not provide arbitrary memory writes or register writes;
those remain behind the authenticated inner KD protocol. Supported commands
are `PING`, `HELP`, `STATUS`, `NIC`, `PCI`, `USB`, `LOADER`, and `ALL`.
The host helper binds the diagnostic port before boot, records automatic and
transport diagnostics immediately, retries commands for up to five minutes by
default, and waits for explicit begin/end records:

```text
python3 scripts/kdnet_harvest.py USB STATUS \
  --output asus-kdnet-harvest.log
```

For a hardware session, keep the same socket open after the requested snapshot
so later boot diagnostics cannot fall into a listener gap:

```text
python3 scripts/kdnet_harvest.py ALL --listen-seconds 3600 \
  --output asus-kdnet-harvest.log
```

`--listen-only` records indefinitely (or for `--listen-seconds`) without
sending a request. This is useful when only automatic phase snapshots are
wanted. Every received datagram is written immediately; a partial or timed-out
command therefore does not discard the diagnostics already harvested.

`USB` filters the PCI snapshot to USB-class controllers and is a distinct
`/KDNET-HARVEST` option; `ALL` runs both the full PCI walk and this searchable
USB-only summary. `PCI` includes every
reachable function and discovers secondary buses through PCI bridges. While
the debugger has the other CPUs frozen on x86/x64, these commands use read-only
PCI configuration mechanism 1 directly. They therefore work at the initial KD
break even when the HAL's PCI bus handlers are not initialized yet. Records
report both the number of matching functions and the total functions found.
While the PnP manager starts devices, the independent `/PNP-HARVEST` option
enables high-volume `PNPHARVEST` records containing the devnode state, complete
device stack, raw and translated resources, IRP target driver, pending wait,
and completion status. The generated menu combines it with the explicitly
verbose entries only. Boot-driver entry failures include their exact status
and image entry point as `DRIVERHARVEST` records.

Diagnostic trace storage is recycled after every successful flush. A long
debugging session therefore continues emitting transport diagnostics instead
of becoming silent after the initial 8 KiB.

The control handshake uses a bounded 30-attempt window. This tolerates several
seconds of bridge setup, userspace debugger startup, or delayed polled receive
DMA without making a NET-debug boot wait indefinitely when no debugger exists.

The transport reserves the NIC through the HAL debug-device interface. The
normal NDIS miniport therefore must not use the selected device while KDNET
is active.

For QEMU user networking, use `10.0.2.2` as the host and `10.0.2.15` as the
target, then start QEMU with `-device e1000,netdev=kdnet -netdev user,id=kdnet`.
The corresponding boot options are:

```text
/DEBUG /DEBUGPORT=NET /HOSTIP=10.0.2.2 /HOSTMAC=52-55-0A-00-02-02 /TARGETIP=10.0.2.15 /PORT=50000 /KEY=1.2.3.4
```

`HOSTMAC` is the fixed QEMU user-network gateway address and avoids waiting for
ARP while the emulator is still servicing early boot.
