#!/usr/bin/env python3
"""Record and request read-only snapshots from the ReactOS KDNET transport."""

from __future__ import annotations

import argparse
import re
import socket
import sys
import time
from pathlib import Path
from typing import Callable


PROTOCOL_PREFIX = b"KDNET-HARVEST/1 "
RESPONSE_RE = re.compile(r"^\[KDNET-HARVEST/(\d+)\] (.*)$")
COMMANDS = ("PING", "HELP", "STATUS", "NIC", "PCI", "USB", "LOADER", "ALL")


def request_snapshot(
    sock: socket.socket,
    target: tuple[str, int],
    command: str,
    startup_timeout: float,
    retry_interval: float,
    response_timeout: float,
    record: Callable[[bytes], list[str]],
) -> list[str]:
    request = PROTOCOL_PREFIX + command.encode("ascii")
    active_id: str | None = None
    lines: list[str] = []
    startup_deadline = time.monotonic() + startup_timeout
    response_deadline: float | None = None
    next_send = 0.0

    while True:
        now = time.monotonic()
        if active_id is None:
            if now >= startup_deadline:
                raise TimeoutError(
                    f"target did not start {command} within {startup_timeout:g}s; "
                    "it may not yet be polling KDNET"
                )
            if now >= next_send:
                sock.sendto(request, target)
                next_send = now + retry_interval
            deadline = min(startup_deadline, next_send)
        else:
            assert response_deadline is not None
            if now >= response_deadline:
                raise TimeoutError(
                    f"target started {command} as request {active_id}, "
                    "but did not finish"
                )
            deadline = response_deadline

        sock.settimeout(max(0.01, min(0.25, deadline - now)))
        try:
            payload, _peer = sock.recvfrom(65535)
        except TimeoutError:
            continue

        # Automatic phase snapshots and transport trace chunks share this port.
        # Record every line immediately, even while waiting for a command reply.
        for text in record(payload):
            match = RESPONSE_RE.match(text)
            if not match:
                continue
            body = match.group(2)
            if (active_id is None and
                    body == f"BEGIN command={command}"):
                active_id = match.group(1)
                response_deadline = time.monotonic() + response_timeout
            if active_id == match.group(1):
                lines.append(text)
                if body == f"END command={command}":
                    return lines


def listen(
    sock: socket.socket,
    duration: float,
    record: Callable[[bytes], list[str]],
) -> None:
    deadline = time.monotonic() + duration if duration > 0 else None
    while deadline is None or time.monotonic() < deadline:
        timeout = 1.0
        if deadline is not None:
            timeout = max(0.01, min(timeout, deadline - time.monotonic()))
        sock.settimeout(timeout)
        try:
            payload, _peer = sock.recvfrom(65535)
        except TimeoutError:
            continue
        record(payload)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("commands", nargs="*", metavar="COMMAND")
    parser.add_argument("--host", default="192.168.250.1", help="local KDNET IPv4 address")
    parser.add_argument("--target", default="192.168.250.2", help="ReactOS target IPv4 address")
    parser.add_argument("--port", type=int, default=50001, help="harvest UDP port")
    parser.add_argument(
        "--startup-timeout", type=float, default=300.0,
        help="seconds to keep retrying each request while the target boots",
    )
    parser.add_argument(
        "--retry-interval", type=float, default=0.25,
        help="seconds between requests until a matching reply begins",
    )
    parser.add_argument("--response-timeout", type=float, default=30.0)
    parser.add_argument(
        "--listen-only", action="store_true",
        help="record automatic snapshots and trace without sending commands",
    )
    parser.add_argument(
        "--listen-seconds", type=float, default=0.0,
        help="keep recording after commands finish (0 means do not wait)",
    )
    parser.add_argument("--output", type=Path, help="also append responses to this file")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    commands = [command.upper() for command in args.commands]
    if not commands and not args.listen_only:
        commands = ["ALL"]
    invalid = [command for command in commands if command not in COMMANDS]
    if invalid:
        print(f"unsupported command(s): {', '.join(invalid)}", file=sys.stderr)
        return 2

    if args.startup_timeout <= 0 or args.retry_interval <= 0:
        print("startup timeout and retry interval must be positive", file=sys.stderr)
        return 2
    if args.response_timeout <= 0 or args.listen_seconds < 0:
        print("response timeout must be positive and listen seconds non-negative",
              file=sys.stderr)
        return 2

    output = args.output.open("a", encoding="utf-8") if args.output else None

    def record(payload: bytes) -> list[str]:
        lines = payload.decode("utf-8", errors="replace").splitlines()
        for line in lines:
            print(line, flush=True)
            if output:
                print(line, file=output)
        if output:
            output.flush()
        return lines

    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
            sock.bind((args.host, args.port))
            print(
                f"kdnet harvest listening on {args.host}:{args.port}; "
                f"target {args.target}:{args.port}",
                file=sys.stderr,
                flush=True,
            )
            for command in commands:
                request_snapshot(
                    sock,
                    (args.target, args.port),
                    command,
                    args.startup_timeout,
                    args.retry_interval,
                    args.response_timeout,
                    record,
                )
            if args.listen_only:
                listen(sock, args.listen_seconds, record)
            elif args.listen_seconds > 0:
                listen(sock, args.listen_seconds, record)
    except KeyboardInterrupt:
        return 0
    except (OSError, TimeoutError) as error:
        print(f"kdnet harvest failed: {error}", file=sys.stderr)
        return 1
    finally:
        if output:
            output.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
