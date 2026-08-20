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

from .client import (
    KdNetClient,
    KdRequestError,
    KdSessionChanged,
    KdTimeout,
    console_event,
    diff_harvest,
)
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
  peek ADDRESS SIZE [PATH]     bounded break/read/resume; print or save bytes
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
  readio ADDRESS SIZE          read a 1-, 2-, or 4-byte I/O port value
  writeio ADDRESS SIZE VALUE   write a 1-, 2-, or 4-byte I/O port value
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


def _continue(
    client: KdNetClient,
    auto_modules: bool,
    harvest_root: Path | None,
    stack_size: int,
    break_timeout: float,
    capture_timeout: float,
    resume_timeout: float,
) -> None:
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
                command = line.strip()
                words = command.split()
                if len(words) == 2 and words[0] == "stall-capture":
                    # A FIFO command that times out must not take the listener
                    # down with it -- same per-command containment as repl().
                    # KdSessionChanged still propagates for the reconnect loop.
                    try:
                        _stall_capture(
                            client,
                            words[1],
                            harvest_root,
                            stack_size,
                            break_timeout,
                            capture_timeout,
                            resume_timeout,
                        )
                    except KdSessionChanged:
                        raise
                    except (KdRequestError, KdTimeout, ProtocolError,
                            RuntimeError, ValueError) as error:
                        print(f"roskd: stall-capture failed: {error}",
                              file=sys.stderr)
                    continue
                if 3 <= len(words) <= 4 and words[0] == "peek":
                    try:
                        address = _integer(words[1])
                        size = _integer(words[2])
                    except argparse.ArgumentTypeError as error:
                        print(f"roskd: {error}", file=sys.stderr)
                        continue
                    destination = Path(words[3]) if len(words) == 4 else None
                    try:
                        _bounded_peek(
                            client,
                            address,
                            size,
                            destination,
                            break_timeout,
                            capture_timeout,
                            resume_timeout,
                        )
                    except KdSessionChanged:
                        raise
                    except (KdRequestError, KdTimeout, ProtocolError,
                            RuntimeError, ValueError) as error:
                        print(f"roskd: peek failed: {error}", file=sys.stderr)
                    continue
                if command in ("break", ""):
                    print("[KD] break-in requested via stdin", file=sys.stderr)
                    try:
                        client.break_in()
                    except KdSessionChanged:
                        raise
                    except (KdRequestError, KdTimeout, ProtocolError) as error:
                        print(f"roskd: break-in failed: {error}",
                              file=sys.stderr)
                        continue
                    return
                print(
                    f"[KD] target running; ignored {command!r} "
                    "(write 'break', 'peek ADDRESS SIZE [PATH]', or "
                    "'stall-capture ID')",
                    file=sys.stderr,
                )
    except KeyboardInterrupt:
        print("\n[KD] break-in requested", file=sys.stderr)
        client.break_in()


def _harvest_path(root: Path) -> Path:
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    return root / stamp


def _auto_harvest(
    client: KdNetClient,
    root: Path | None,
    stack_size: int,
    *,
    deadline: float | None = None,
) -> tuple[Path, bool] | None:
    if root is None:
        client._emit("harvest-failed", "automatic harvesting is disabled")
        return None
    if not client.stopped:
        client._emit("harvest-failed", "target is not stopped")
        return None
    try:
        destination = client.harvest(
            _harvest_path(root), stack_size=stack_size, deadline=deadline
        )
        print(f"[KD] harvested {destination}", file=sys.stderr)
        return destination, bool(getattr(client, "last_harvest_complete", True))
    except (
        KdRequestError,
        KdTimeout,
        OSError,
        ProtocolError,
        RuntimeError,
        ValueError,
    ) as error:
        client._emit("harvest-failed", str(error))
        print(f"[KD] harvest failed: {error}", file=sys.stderr)
        return None


def _transaction_event(
    client: KdNetClient, transaction_id: str, phase: str, detail: str = ""
) -> None:
    suffix = f" {detail}" if detail else ""
    client._emit("transaction", f"id={transaction_id} phase={phase}{suffix}")


def _stall_capture(
    client: KdNetClient,
    transaction_id: str,
    harvest_root: Path | None,
    stack_size: int,
    break_timeout: float,
    capture_timeout: float,
    resume_timeout: float,
) -> None:
    stopped_by_transaction = False
    _transaction_event(client, transaction_id, "break-requested")
    try:
        try:
            with client.operation_deadline(time.monotonic() + break_timeout):
                client.break_in(timeout=break_timeout)
        except KdTimeout as error:
            # The outer break packet was sent, so a lost or delayed state-change
            # may already have stopped the target.  Give that stop one bounded
            # recovery window before declaring the session unusable.
            _transaction_event(
                client, transaction_id, "break-uncertain", f"error={error}"
            )
            with client.operation_deadline(time.monotonic() + resume_timeout):
                client.wait_for_stop(
                    auto_continue_modules=True, timeout=resume_timeout
                )
            client._manual_break_stop = True
            _transaction_event(client, transaction_id, "stopped-late")
        stopped_by_transaction = True
        _transaction_event(client, transaction_id, "stopped")
        harvest = _auto_harvest(
            client,
            harvest_root,
            stack_size,
            deadline=time.monotonic() + capture_timeout,
        )
        if harvest is None:
            _transaction_event(client, transaction_id, "harvest-failed")
        else:
            destination, complete = harvest
            phase = "harvest-finished" if complete else "harvest-partial"
            _transaction_event(client, transaction_id, phase, f"path={destination}")
    except Exception as error:
        phase = "harvest-failed" if stopped_by_transaction else "break-failed"
        _transaction_event(client, transaction_id, phase, f"error={error}")
        raise
    finally:
        if stopped_by_transaction and client.stopped:
            try:
                with client.operation_deadline(time.monotonic() + resume_timeout):
                    client.drain_pending_request(timeout=resume_timeout)
                    client.continue_execution()
            except Exception as error:
                phase = "resume-uncertain" if not client.stopped else "resume-failed"
                _transaction_event(
                    client, transaction_id, phase, f"error={error}"
                )
                raise
            else:
                _transaction_event(client, transaction_id, "resume-sent")
        elif stopped_by_transaction:
            _transaction_event(
                client,
                transaction_id,
                "resume-uncertain",
                "error=target no longer marked stopped",
            )


def _bounded_peek(
    client: KdNetClient,
    address: int,
    size: int,
    destination: Path | None,
    break_timeout: float,
    capture_timeout: float,
    resume_timeout: float,
) -> bytes:
    """Read one virtual-memory range while bounding the target stop time."""
    if size < 1 or size > 0x10000:
        raise ValueError("peek size must be between 1 and 65536 bytes")

    transaction_id = f"peek-{time.monotonic_ns():x}"
    stopped_by_transaction = False
    data: bytes
    started = time.monotonic()
    _transaction_event(client, transaction_id, "break-requested")
    try:
        try:
            with client.operation_deadline(time.monotonic() + break_timeout):
                client.break_in(timeout=break_timeout)
        except KdTimeout as error:
            _transaction_event(
                client, transaction_id, "break-uncertain", f"error={error}"
            )
            with client.operation_deadline(time.monotonic() + resume_timeout):
                client.wait_for_stop(
                    auto_continue_modules=True, timeout=resume_timeout
                )
            client._manual_break_stop = True
            _transaction_event(client, transaction_id, "stopped-late")
        stopped_by_transaction = True
        _transaction_event(client, transaction_id, "stopped")
        with client.operation_deadline(time.monotonic() + capture_timeout):
            data = client.read_virtual(address, size)
        if len(data) != size:
            raise RuntimeError(
                f"short peek at 0x{address:x}: requested {size}, received {len(data)}"
            )
        _transaction_event(
            client,
            transaction_id,
            "capture-finished",
            f"address=0x{address:x} size={size}",
        )
    except Exception as error:
        phase = "capture-failed" if stopped_by_transaction else "break-failed"
        _transaction_event(client, transaction_id, phase, f"error={error}")
        raise
    finally:
        if stopped_by_transaction and client.stopped:
            try:
                with client.operation_deadline(time.monotonic() + resume_timeout):
                    client.drain_pending_request(timeout=resume_timeout)
                    client.continue_execution()
            except Exception as error:
                phase = "resume-uncertain" if not client.stopped else "resume-failed"
                _transaction_event(
                    client, transaction_id, phase, f"error={error}"
                )
                raise
            else:
                _transaction_event(client, transaction_id, "resume-sent")

    stopped_ms = (time.monotonic() - started) * 1000.0
    if destination is None:
        print(_hexdump(data, address))
    else:
        destination.write_bytes(data)
        print(f"peek: saved {len(data)} bytes to {destination}")
    print(f"peek: target resumed after {stopped_ms:.0f} ms")
    return data


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


def _read_command(client: KdNetClient, prompt: str) -> str:
    """Read one REPL command, servicing the KDNET socket while waiting.

    input() blocks, and nothing else in this process reads the socket, so a
    target that reboots while the REPL waits for its next command used to be
    invisible: the reconnect loop in main() only ever sees KdSessionChanged
    raised from inside a receive.  A backgrounded listener driven through a
    FIFO spends nearly all of its life in this wait, which is exactly when an
    unattended machine gets powered on.
    """
    sys.stdout.write(prompt)
    sys.stdout.flush()
    interactive = sys.stdin.isatty()
    while True:
        if select.select([sys.stdin], [], [], 0)[0]:
            line = sys.stdin.readline()
            if not line:
                # On a terminal an empty read is Ctrl-D: end the session.  On a
                # FIFO it only means "no writer is attached right now", which
                # happens after *every* command when the driver opens, writes
                # and closes the pipe.  Treating that as EOF used to end the
                # REPL after a single scripted command and strand the target
                # halted at its break, with nothing left reading stdin to
                # resume it.  The stdin watcher has always ignored it; so does
                # this now.
                if interactive:
                    raise EOFError
                client.poll_idle(0.5)
                continue
            return line
        client.poll_idle(0.5)


def repl(
    client: KdNetClient,
    auto_modules: bool,
    harvest_root: Path | None,
    stack_size: int,
    break_timeout: float,
    capture_timeout: float,
    resume_timeout: float,
) -> int:
    watches: list[tuple[int, int, str]] = []
    print(HELP)
    while True:
        try:
            line = _read_command(client, "roskd> ")
        except EOFError:
            print()
            if client.stopped:
                client.continue_execution()
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
                _continue(
                    client,
                    auto_modules,
                    harvest_root,
                    stack_size,
                    break_timeout,
                    capture_timeout,
                    resume_timeout,
                )
                _auto_harvest(
                    client,
                    harvest_root,
                    stack_size,
                    deadline=time.monotonic() + capture_timeout,
                )
            elif command == "break":
                client.break_in(timeout=break_timeout)
                _auto_harvest(
                    client,
                    harvest_root,
                    stack_size,
                    deadline=time.monotonic() + capture_timeout,
                )
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
            elif command == "readio" and len(args) == 2:
                address, size = (_integer(value) for value in args)
                value = client.read_io(address, size)
                print(f"0x{address:x}: 0x{value:0{size * 2}x}")
            elif command == "writeio" and len(args) == 3:
                address, size, value = (_integer(value) for value in args)
                written = client.write_io(address, size, value)
                print(f"wrote {written} byte(s) to I/O port 0x{address:x}")
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
                # Never walk away from a halted target: without this the
                # machine stays frozen at its break with nobody left to
                # resume it.
                if client.stopped:
                    client.continue_execution()
                return 0
            else:
                print("unknown command or wrong arguments; enter `help`", file=sys.stderr)
        except KdSessionChanged:
            raise
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
        "--session-state", type=Path,
        help="securely persist KDNET session keys across debugger restarts",
    )
    parser.add_argument(
        "--log", type=Path, default=Path("roskd.log"),
        help="append decoded output and stop events to this file",
    )
    parser.add_argument(
        "--timeout", type=float, default=3600.0,
        help="seconds to wait for the target's initial handshake",
    )
    parser.add_argument(
        "--break-timeout", type=float, default=5.0,
        help="seconds allowed for an automated break-in",
    )
    parser.add_argument(
        "--capture-timeout", type=float, default=10.0,
        help="overall seconds allowed for an automatic harvest",
    )
    parser.add_argument(
        "--resume-timeout", type=float, default=5.0,
        help="seconds allowed for an automated resume",
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
    parser.add_argument(
        "--diff-harvest", type=Path, default=None,
        help="OFFLINE: diff a captured fault page in this harvest dir against an "
             "on-disk PE (with --diff-module); does not touch the network",
    )
    parser.add_argument(
        "--diff-module", type=Path, default=None,
        help="on-disk PE image (e.g. build .../setup.exe) to diff against",
    )
    parser.add_argument(
        "--diff-region", default="fault-code-page",
        help="captured fault page region name to diff (default: fault-code-page)",
    )
    return parser.parse_args(argv)


def _run_diff(args: argparse.Namespace) -> int:
    """Offline fault-page vs on-disk-image diff. Never opens the KDNET socket."""
    if args.diff_module is None:
        print("roskd: --diff-harvest requires --diff-module", file=sys.stderr)
        return 2
    try:
        result = diff_harvest(args.diff_harvest, args.diff_module, args.diff_region)
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"roskd: diff failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2))
    print(f"\n{result['verdict']}", file=sys.stderr)
    return 1 if result["real_corruption_count"] else 0


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.diff_harvest is not None:
        return _run_diff(args)
    if (
        not 1 <= args.port <= 65535
        or args.timeout <= 0
        or args.break_timeout <= 0
        or args.capture_timeout <= 0
        or args.resume_timeout <= 0
        or args.stack_bytes <= 0
    ):
        print("roskd: port, timeouts, and stack size must be positive", file=sys.stderr)
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
            session_state=args.session_state,
        ) as client:
            print(
                f"[KDNET] listening on {args.host}:{args.port}; "
                f"target {args.target}:{args.port}",
                file=sys.stderr,
                flush=True,
            )
            while True:
                try:
                    client.connect(args.timeout)
                    print(f"[KDNET] connected: {client.version}", file=sys.stderr)
                    if not args.no_auto_continue:
                        _continue(
                            client,
                            not args.no_auto_modules,
                            harvest_root,
                            args.stack_bytes,
                            args.break_timeout,
                            args.capture_timeout,
                            args.resume_timeout,
                        )
                    _auto_harvest(
                        client,
                        harvest_root,
                        args.stack_bytes,
                        deadline=time.monotonic() + args.capture_timeout,
                    )
                    return repl(
                        client,
                        not args.no_auto_modules,
                        harvest_root,
                        args.stack_bytes,
                        args.break_timeout,
                        args.capture_timeout,
                        args.resume_timeout,
                    )
                except KdSessionChanged as error:
                    print(f"[KDNET] reconnecting: {error}", file=sys.stderr)
    except KeyboardInterrupt:
        print("\nroskd: interrupted", file=sys.stderr)
        return 130
    except (
        ImportError,
        OSError,
        KdTimeout,
        KdSessionChanged,
        ProtocolError,
        KdRequestError,
    ) as error:
        print(f"roskd: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
