"""Friendly command-line interface for the native Python KDNET client."""

from __future__ import annotations

import argparse
import json
import select
import shlex
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

from .client import KdNetClient, KdRequestError, KdTimeout, console_event
from .protocol import ProtocolError


def _integer(value: str) -> int:
    try:
        return int(value, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"invalid integer: {value}") from error


def _hexdump(data: bytes, base: int = 0) -> str:
    lines = []
    for offset in range(0, len(data), 16):
        chunk = data[offset:offset + 16]
        hexadecimal = " ".join(f"{value:02x}" for value in chunk)
        printable = "".join(chr(value) if 32 <= value < 127 else "." for value in chunk)
        lines.append(
            f"{base + offset:016x}  {hexadecimal:<47}  |{printable}|"
        )
    return "\n".join(lines)


def _print_registers(registers: dict[str, int]) -> None:
    rows = (
        ("rax", "rbx", "rcx", "rdx"),
        ("rsi", "rdi", "rbp", "rsp"),
        ("r8", "r9", "r10", "r11"),
        ("r12", "r13", "r14", "r15"),
        ("rip", "rflags", "dr6", "dr7"),
    )
    for row in rows:
        print("  ".join(
            f"{name:6s}=0x{registers[name]:016x}"
            for name in row if name in registers
        ))


HELP = """Commands:
  c | continue                 continue and wait for the next real stop
  break                        send a KD break-in and wait for a stop
  watch ADDRESS SIZE [LABEL]   register a memory region for `sample` dumps
  unwatch                      clear all watched regions
  sample [COUNT [INTERVAL]]    break, dump per-CPU RIP/RSP and watched
                               regions, resume immediately; repeat COUNT
                               times INTERVAL seconds apart (default 1, 10)
  regs                         display AMD64 registers
  modules                      list loaded kernel modules
  dmesg [PATH]                 display or save the buffered target DbgPrint log
  harvest [PATH] [STACK_BYTES] save context, stack, code, modules, and KDBG data
  read ADDRESS SIZE            read and hex-dump virtual memory
  readphys ADDRESS SIZE        read and hex-dump physical memory
  write ADDRESS HEXBYTES       write virtual memory (example: write ADDR 90cc)
  bp ADDRESS                   set a software breakpoint
  bc HANDLE                    clear a software breakpoint
  query ADDRESS                query the target virtual address
  status                       show connection, target version, and last stop
  version                      show the KD target version
  help                         show this help
  detach                       continue the target and exit
  quit                         exit while leaving the target stopped

Ctrl-C while `continue` is waiting sends a KD break-in. Writing `break` (or
a blank line) to stdin while the target runs does the same, which is how a
scripted driver on a FIFO breaks in without signals.
"""


def _continue(client: KdNetClient, auto_modules: bool) -> None:
    if client.stopped:
        client.continue_execution()
    try:
        # Poll stdin between short waits so a scripted driver (a FIFO on
        # stdin) can request a break-in by writing `break` -- signals are
        # unreliable for backgrounded listeners (SIGINT arrives ignored).
        while True:
            try:
                client.wait_for_stop(
                    auto_continue_modules=auto_modules, timeout=0.5
                )
                return
            except KdTimeout:
                if not select.select([sys.stdin], [], [], 0)[0]:
                    continue
                line = sys.stdin.readline()
                if not line:
                    continue
                if line.strip() in ("break", ""):
                    print("[KD] break-in requested via stdin", file=sys.stderr)
                    client.break_in()
                    return
                print(
                    f"[KD] target running; ignored {line.strip()!r} "
                    "(write 'break' first)",
                    file=sys.stderr,
                )
    except KeyboardInterrupt:
        print("\n[KD] break-in requested", file=sys.stderr)
        client.break_in()


def _harvest_path(root: Path) -> Path:
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    return root / stamp


def _auto_harvest(
    client: KdNetClient, root: Path | None, stack_size: int
) -> None:
    if root is None or not client.stopped:
        return
    try:
        destination = client.harvest(_harvest_path(root), stack_size=stack_size)
        print(f"[KD] harvested {destination}", file=sys.stderr)
    except (KdRequestError, KdTimeout, ProtocolError, RuntimeError, ValueError) as error:
        print(f"[KD] harvest failed: {error}", file=sys.stderr)


def _sample_once(
    client: KdNetClient, watches: list[tuple[int, int, str]]
) -> None:
    """Break in, grab the cheapest useful state, and resume before doing any
    formatting, so the target is stopped for as few KD round trips as
    possible (registers per CPU plus the watched regions; no harvest)."""
    was_stopped = client.stopped
    t0 = time.monotonic()
    if not was_stopped:
        client.break_in()
    cpu_count = client.current_state.cpu_count if client.current_state else 1
    contexts = []
    for cpu in range(cpu_count):
        try:
            contexts.append(client.get_registers(cpu))
        except (KdRequestError, KdTimeout, ProtocolError) as error:
            contexts.append({"error": str(error)})
    grabs: list[tuple[str, int, bytes]] = []
    for address, size, label in watches:
        try:
            grabs.append((label, address, client.read_virtual(address, size)))
        except (KdRequestError, KdTimeout, ProtocolError) as error:
            print(f"sample: watch {label} failed: {error}", file=sys.stderr)
    if not was_stopped:
        client.continue_execution()
    stopped_ms = (time.monotonic() - t0) * 1000.0
    stamp = datetime.now(timezone.utc).strftime("%H:%M:%S.%f")[:-3]
    print(f"-- sample {stamp} stopped {stopped_ms:.0f} ms"
          f"{' (target was already stopped)' if was_stopped else ''} --")
    for cpu, registers in enumerate(contexts):
        if "error" in registers:
            print(f"  cpu{cpu}: <{registers['error']}>")
        else:
            print(f"  cpu{cpu}: rip=0x{registers['rip']:016x}"
                  f" rsp=0x{registers['rsp']:016x}")
    for label, address, data in grabs:
        words = [
            str(int.from_bytes(data[i:i + 4], "little"))
            for i in range(0, len(data) - len(data) % 4, 4)
        ]
        print(f"  {label} @0x{address:x}: {data.hex()}"
              + (f"  u32[{' '.join(words)}]" if words else ""))


def _sample_loop(
    client: KdNetClient,
    watches: list[tuple[int, int, str]],
    count: int,
    interval: float,
    auto_modules: bool,
) -> None:
    for index in range(count):
        _sample_once(client, watches)
        if index == count - 1 or client.stopped:
            break
        try:
            # Keep servicing module-load state changes between samples; a
            # plain sleep would leave the target blocked on an unacknowledged
            # state change for the whole interval.
            state = client.wait_for_stop(
                auto_continue_modules=auto_modules, timeout=interval
            )
            print(f"sample: target stopped on its own ({state.kind}); "
                  f"aborting sample loop")
            return
        except KdTimeout:
            continue


def repl(
    client: KdNetClient,
    auto_modules: bool,
    harvest_root: Path | None,
    stack_size: int,
) -> int:
    watches: list[tuple[int, int, str]] = []
    print(HELP)
    while True:
        try:
            line = input("roskd> ")
        except EOFError:
            print()
            return 0
        except KeyboardInterrupt:
            print()
            continue
        try:
            words = shlex.split(line)
        except ValueError as error:
            print(error, file=sys.stderr)
            continue
        if not words:
            continue
        command, *args = words
        try:
            if command in ("c", "continue"):
                _continue(client, auto_modules)
                _auto_harvest(client, harvest_root, stack_size)
            elif command == "break":
                client.break_in()
                _auto_harvest(client, harvest_root, stack_size)
            elif command == "watch" and 2 <= len(args) <= 3:
                address, size = _integer(args[0]), _integer(args[1])
                label = args[2] if len(args) == 3 else f"0x{address:x}"
                watches.append((address, size, label))
                print(f"watching {label}: 0x{address:x} +{size}")
            elif command == "unwatch":
                watches.clear()
                print("watches cleared")
            elif command == "sample" and len(args) <= 2:
                count = _integer(args[0]) if args else 1
                interval = float(args[1]) if len(args) == 2 else 10.0
                _sample_loop(client, watches, count, interval, auto_modules)
            elif command == "regs":
                _print_registers(client.get_registers())
            elif command == "modules":
                for module in client.get_loaded_modules():
                    name = module.base_name or module.full_name or "<unnamed>"
                    print(
                        f"0x{module.dll_base:016x}-"
                        f"0x{module.dll_base + module.size:016x} {name}"
                    )
            elif command == "dmesg" and len(args) <= 1:
                buffered_log, _, metadata = client.get_debug_print_log()
                if args:
                    destination = Path(args[0])
                    destination.write_bytes(buffered_log)
                    print(
                        f"saved {len(buffered_log)} bytes to {destination} "
                        f"(rollovers={metadata['rollover_count']})"
                    )
                else:
                    print(buffered_log.decode("utf-8", errors="replace"), end="")
                    if buffered_log and not buffered_log.endswith(b"\n"):
                        print()
            elif command == "harvest" and len(args) <= 2:
                destination = (
                    Path(args[0])
                    if args
                    else _harvest_path(harvest_root or Path("roskd-harvest"))
                )
                requested_stack = _integer(args[1]) if len(args) == 2 else stack_size
                print(f"harvested {client.harvest(destination, stack_size=requested_stack)}")
            elif command in ("read", "readphys") and len(args) == 2:
                address, size = (_integer(value) for value in args)
                data = (
                    client.read_virtual(address, size)
                    if command == "read"
                    else client.read_physical(address, size)
                )
                print(_hexdump(data, address))
                if len(data) != size:
                    print(f"short read: requested {size}, received {len(data)}")
            elif command == "write" and len(args) == 2:
                address = _integer(args[0])
                data = bytes.fromhex(args[1])
                print(f"wrote {client.write_virtual(address, data)} byte(s)")
            elif command == "bp" and len(args) == 1:
                handle = client.set_breakpoint(_integer(args[0]))
                print(f"breakpoint handle {handle}")
            elif command == "bc" and len(args) == 1:
                client.clear_breakpoint(_integer(args[0]))
            elif command == "query" and len(args) == 1:
                address_space, flags = client.query_memory(_integer(args[0]))
                print(f"address_space={address_space} flags=0x{flags:08x}")
            elif command == "status":
                print(json.dumps(client.status(), indent=2))
            elif command == "version":
                print(client.version)
            elif command in ("help", "?"):
                print(HELP)
            elif command == "detach":
                if client.stopped:
                    client.continue_execution()
                return 0
            elif command in ("quit", "q", "exit"):
                return 0
            else:
                print("unknown command or wrong arguments; enter `help`", file=sys.stderr)
        except (KdRequestError, KdTimeout, ProtocolError, RuntimeError, ValueError) as error:
            print(f"roskd: {error}", file=sys.stderr)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Native Python KDNET debugger for ReactOS"
    )
    parser.add_argument("--host", default="192.168.250.1", help="local IPv4 address")
    parser.add_argument("--target", default="192.168.250.2", help="target IPv4 address")
    parser.add_argument("--port", default=50000, type=int, help="KDNET UDP port")
    parser.add_argument("--key", default="1.2.3.4", help="four-part KDNET key")
    parser.add_argument(
        "--log", type=Path, default=Path("roskd.log"),
        help="append decoded output and stop events to this file",
    )
    parser.add_argument(
        "--timeout", type=float, default=3600.0,
        help="seconds to wait for the target's initial handshake",
    )
    parser.add_argument(
        "--no-auto-modules", action="store_true",
        help="stop on module load/unload state changes",
    )
    parser.add_argument(
        "--no-auto-continue", action="store_true",
        help="remain at the initial boot breakpoint",
    )
    parser.add_argument(
        "--harvest-dir", type=Path, default=Path("roskd-harvest"),
        help="automatically save a structured snapshot at debugger stops",
    )
    parser.add_argument(
        "--no-harvest", action="store_true",
        help="disable automatic structured snapshots",
    )
    parser.add_argument(
        "--stack-bytes", type=_integer, default=0x4000,
        help="stack bytes saved by each harvest (default: 0x4000)",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if not 1 <= args.port <= 65535 or args.timeout <= 0 or args.stack_bytes <= 0:
        print("roskd: port, timeout, and stack size must be positive", file=sys.stderr)
        return 2
    harvest_root = None if args.no_harvest else args.harvest_dir
    try:
        with KdNetClient(
            args.host,
            args.target,
            args.port,
            args.key,
            log=args.log,
            event_callback=console_event,
        ) as client:
            print(
                f"[KDNET] listening on {args.host}:{args.port}; "
                f"target {args.target}:{args.port}",
                file=sys.stderr,
                flush=True,
            )
            client.connect(args.timeout)
            print(f"[KDNET] connected: {client.version}", file=sys.stderr)
            if not args.no_auto_continue:
                _continue(client, not args.no_auto_modules)
            _auto_harvest(client, harvest_root, args.stack_bytes)
            return repl(
                client,
                not args.no_auto_modules,
                harvest_root,
                args.stack_bytes,
            )
    except KeyboardInterrupt:
        print("\nroskd: interrupted", file=sys.stderr)
        return 130
    except (ImportError, OSError, KdTimeout, ProtocolError, KdRequestError) as error:
        print(f"roskd: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
