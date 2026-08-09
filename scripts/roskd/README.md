# Native Python KDNET debugger

`roskd` is a small host-side debugger for the ReactOS KDNET transport. It does
not require Radare2 or WinDbg. The implementation includes the authenticated
KDNET handshake, encrypted UDP packets, the inner KD framing protocol, and the
AMD64 manipulate requests needed to inspect a stopped kernel.

The host must start listening before ReactOS boots. For the ASUS boot-menu
configuration in this tree, run:

```console
python3 scripts/roskd_cli.py \
    --host 192.168.250.1 \
    --target 192.168.250.2 \
    --port 50000 \
    --key 1.2.3.4 \
    --log asus-kdnet.log \
    --harvest-dir asus-kdnet-harvest
```

KDNET uses the static addresses and destination MAC configured by FreeLdr. It
does not use DHCP, DNS, TCP, or an operating-system network driver. The host
interface must already own `192.168.250.1/24`, and its Ethernet MAC must match
the `HOSTMAC` boot option.

The `cryptography` Python package is the only non-standard dependency. On
Debian-family systems it is available as `python3-cryptography`.

## Interactive commands

The client automatically acknowledges debug output and module events. It
continues past module events and stops at exceptions. Press Ctrl-C while it is
running to send a KD break-in; unlike a process interrupt, this leaves the
client alive to acknowledge and inspect the resulting state-change packet.
When continuing from that explicit break-in on AMD64, the client also corrects
ReactOS's rewound `int3` RIP before sending Continue. This prevents the target
from immediately stopping on the same break instruction again; target-raised
exceptions and debugger-planted breakpoints are not adjusted.

Useful commands include:

```text
regs
modules
read ADDRESS SIZE
write ADDRESS HEXBYTES
readphys ADDRESS SIZE
query ADDRESS
bp ADDRESS
bc HANDLE
harvest [PATH] [STACK_BYTES]
c
```

Use `--no-auto-continue` to remain at the first kernel/module stop. Use
`--no-auto-modules` to stop at every subsequent load/unload event. Automatic
harvesting is enabled by default and can be disabled with `--no-harvest`.

## Structured harvesting

At every debugger stop, the default harvest saves data before returning the
prompt:

- a JSON manifest containing the KD state, target version, registers, observed
  modules, and possible code addresses found on the raw stack;
- the complete AMD64 `CONTEXT` and KD state-change packet;
- a code window around RIP and a page-bounded, progressive stack capture
  beginning at the exact RSP;
- the current thread object, kernel image header, and list heads;
- every registered debugger-data block, including the full ReactOS `KDBG`
  block.

The module list in an automatic harvest comes from load-symbol events already
received during boot. This avoids hundreds of synchronous memory requests and
keeps a typical stop harvest near ten seconds. The interactive `modules`
command performs a live `PsLoadedModuleList` walk when an exact current list is
needed. `manifest.json` is replaced atomically after each successful region.
Stack chunks are flushed individually, so an unmapped guard page ends the
capture without discarding the readable portion or the earlier core snapshot.

The raw files intentionally remain useful without symbols. With the matching
ReactOS build tree, module offsets and stack candidates can subsequently be
resolved against the local ELF/PE debug artifacts.

## Isolated QEMU test

An isolated TAP avoids changing the LAN configuration:

```console
sudo ip tuntap add dev tapkdq mode tap user "$USER"
sudo ip addr add 10.0.2.2/24 dev tapkdq
sudo ip link set tapkdq up

python3 scripts/roskd_cli.py \
    --host 10.0.2.2 --target 10.0.2.15 --port 50000 --key 1.2.3.4 \
    --log qemu-kdnet.log --harvest-dir qemu-kdnet-harvest

qemu-system-x86_64 -enable-kvm -cpu host -smp 4 -m 4096 \
    -boot order=d -cdrom build_nt10_dual_cmake/bootcd.iso \
    -netdev tap,id=n0,ifname=tapkdq,script=no,downscript=no \
    -device e1000,netdev=n0,mac=52:54:00:12:34:56
```

Select `ReactOS NT10 - QEMU e1000 KDNET` in FreeLdr.

## Current scope

Register and kernel-structure decoding currently targets AMD64. The client has
no PDB parser or source-level unwinder yet, and it is not a replacement for all
WinDbg commands. Virtual memory, context, breakpoints, continue/break-in, and
raw structure harvesting work end to end. Physical-memory requests are
implemented, but a target may return `STATUS_UNSUCCESSFUL` when its KD backend
does not support them.
