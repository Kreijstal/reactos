"""High-level ReactOS KDNET debugger session."""

from __future__ import annotations

import secrets
import json
import socket
import struct
import sys
import time
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, Iterable, TextIO

from .protocol import (
    KD_INITIAL_ID,
    KD_LEADER_CONTROL,
    KD_LEADER_DATA,
    KD_SYNC_ID,
    KD_TYPE_ACKNOWLEDGE,
    KD_TYPE_DEBUG_IO,
    KD_TYPE_FILE_IO,
    KD_TYPE_RESEND,
    KD_TYPE_STATE_CHANGE64,
    KD_TYPE_STATE_MANIPULATE,
    KD_TYPE_UNUSED,
    KDNET_DIRECTION_DEBUGGER,
    KDNET_DIRECTION_TARGET,
    KDNET_TYPE_CONTROL,
    KDNET_TYPE_DATA,
    KdNetCrypto,
    KdPacket,
    ProtocolError,
    decode_kd_packet,
    encode_kd_control,
    encode_kd_data,
)


DBGKD_EXCEPTION_STATE_CHANGE = 0x3030
DBGKD_LOAD_SYMBOLS_STATE_CHANGE = 0x3031
DBGKD_COMMAND_STRING_STATE_CHANGE = 0x3032
DBGKD_PRINT_STRING_API = 0x3230
DBGKD_GET_STRING_API = 0x3231

DBGKD_READ_VIRTUAL_MEMORY_API = 0x3130
DBGKD_WRITE_VIRTUAL_MEMORY_API = 0x3131
DBGKD_GET_CONTEXT_API = 0x3132
DBGKD_SET_CONTEXT_API = 0x3133
DBGKD_WRITE_BREAKPOINT_API = 0x3134
DBGKD_RESTORE_BREAKPOINT_API = 0x3135
DBGKD_CONTINUE_API = 0x3136
DBGKD_READ_PHYSICAL_MEMORY_API = 0x313D
DBGKD_WRITE_PHYSICAL_MEMORY_API = 0x313E
DBGKD_GET_VERSION_API = 0x3146
DBGKD_QUERY_MEMORY_API = 0x315C

DBG_CONTINUE = 0x00010002
STATUS_BREAKPOINT = 0x80000003
STATUS_SUCCESS = 0
STATUS_NO_SUCH_FILE = 0xC000000F
KD_MANIPULATE_SIZE = 56
KD_FILE_IO_SIZE = 64
KD_TRANSFER_MAX = 0x800
CONTEXT_AMD64_ALL = 0x0010003F


class KdTimeout(TimeoutError):
    """The target did not produce the expected KD packet in time."""


class KdRequestError(RuntimeError):
    """The target rejected a KD manipulate request."""

    def __init__(self, api: int, status: int):
        super().__init__(f"KD request 0x{api:04x} failed with 0x{status:08x}")
        self.api = api
        self.status = status


@dataclass(frozen=True)
class KdStateChange:
    state: int
    cpu_level: int
    cpu: int
    cpu_count: int
    thread: int
    program_counter: int
    exception_code: int | None = None
    exception_address: int | None = None
    module_path: str | None = None
    module_base: int | None = None
    module_size: int | None = None
    unloading: bool = False

    @property
    def kind(self) -> str:
        return {
            DBGKD_EXCEPTION_STATE_CHANGE: "exception",
            DBGKD_LOAD_SYMBOLS_STATE_CHANGE: "module",
            DBGKD_COMMAND_STRING_STATE_CHANGE: "command",
        }.get(self.state, f"state-0x{self.state:x}")


@dataclass(frozen=True)
class KdVersion:
    major: int
    build: int
    protocol_major: int
    protocol_minor: int
    flags: int
    machine: int
    kernel_base: int
    loaded_module_list: int
    debugger_data_list: int

    @property
    def is_64bit(self) -> bool:
        return self.machine == 0x8664 or bool(self.flags & 0x0004)


@dataclass(frozen=True)
class KdModule:
    entry: int
    dll_base: int
    entry_point: int
    size: int
    full_name: str
    base_name: str


@dataclass(frozen=True)
class KdDebuggerDataBlock:
    address: int
    owner_tag: str
    size: int
    data: bytes


EventCallback = Callable[[str, object], None]


def parse_state_change(payload: bytes) -> KdStateChange:
    if len(payload) < 32:
        raise ProtocolError("truncated KD state-change payload")
    state, cpu_level, cpu, cpu_count = struct.unpack_from("<IHHI", payload)
    thread, pc = struct.unpack_from("<QQ", payload, 16)
    values: dict[str, object] = {}
    if state == DBGKD_EXCEPTION_STATE_CHANGE and len(payload) >= 56:
        values["exception_code"] = struct.unpack_from("<I", payload, 32)[0]
        values["exception_address"] = struct.unpack_from("<Q", payload, 48)[0]
    elif state == DBGKD_LOAD_SYMBOLS_STATE_CHANGE and len(payload) >= 65:
        path_length = struct.unpack_from("<I", payload, 32)[0]
        values["module_base"] = struct.unpack_from("<Q", payload, 40)[0]
        values["module_size"] = struct.unpack_from("<I", payload, 60)[0]
        values["unloading"] = bool(payload[64])
        if path_length and path_length <= len(payload) - 65:
            raw_path = payload[-path_length:].rstrip(b"\0")
            try:
                values["module_path"] = raw_path.decode("utf-8")
            except UnicodeDecodeError:
                values["module_path"] = raw_path.decode("utf-16-le", errors="replace")
    return KdStateChange(
        state=state,
        cpu_level=cpu_level,
        cpu=cpu,
        cpu_count=cpu_count,
        thread=thread,
        program_counter=pc,
        **values,
    )


def parse_amd64_context(context: bytes) -> dict[str, int]:
    """Return the commonly useful fields from an AMD64 CONTEXT record."""
    offsets = {
        "mxcsr": (0x34, "I"),
        "cs": (0x38, "H"),
        "ss": (0x42, "H"),
        "rflags": (0x44, "I"),
        "dr0": (0x48, "Q"),
        "dr1": (0x50, "Q"),
        "dr2": (0x58, "Q"),
        "dr3": (0x60, "Q"),
        "dr6": (0x68, "Q"),
        "dr7": (0x70, "Q"),
        "rax": (0x78, "Q"),
        "rcx": (0x80, "Q"),
        "rdx": (0x88, "Q"),
        "rbx": (0x90, "Q"),
        "rsp": (0x98, "Q"),
        "rbp": (0xA0, "Q"),
        "rsi": (0xA8, "Q"),
        "rdi": (0xB0, "Q"),
        "r8": (0xB8, "Q"),
        "r9": (0xC0, "Q"),
        "r10": (0xC8, "Q"),
        "r11": (0xD0, "Q"),
        "r12": (0xD8, "Q"),
        "r13": (0xE0, "Q"),
        "r14": (0xE8, "Q"),
        "r15": (0xF0, "Q"),
        "rip": (0xF8, "Q"),
    }
    result: dict[str, int] = {}
    for name, (offset, kind) in offsets.items():
        size = struct.calcsize(kind)
        if offset + size <= len(context):
            result[name] = struct.unpack_from("<" + kind, context, offset)[0]
    return result


def parse_amd64_loader_entry(entry: int, data: bytes) -> dict[str, int]:
    """Decode the fixed part of a 64-bit kernel loader entry."""
    if len(data) < 0x68:
        raise ProtocolError("truncated AMD64 loader entry")
    return {
        "entry": entry,
        "flink": struct.unpack_from("<Q", data, 0)[0],
        "blink": struct.unpack_from("<Q", data, 8)[0],
        "dll_base": struct.unpack_from("<Q", data, 0x30)[0],
        "entry_point": struct.unpack_from("<Q", data, 0x38)[0],
        "size": struct.unpack_from("<I", data, 0x40)[0],
        "full_name_length": struct.unpack_from("<H", data, 0x48)[0],
        "full_name_buffer": struct.unpack_from("<Q", data, 0x50)[0],
        "base_name_length": struct.unpack_from("<H", data, 0x58)[0],
        "base_name_buffer": struct.unpack_from("<Q", data, 0x60)[0],
    }


class KdNetClient:
    """A synchronous, scriptable KDNET debugger.

    The client is intended to be created before the target boots. ``connect``
    waits for the target poke, performs the encrypted handshake, breaks in,
    acknowledges the initial state change, and queries the target version.
    """

    def __init__(
        self,
        host: str,
        target: str,
        port: int,
        key: str,
        *,
        log: Path | str | TextIO | None = None,
        event_callback: EventCallback | None = None,
    ):
        self.host = host
        self.target = target
        self.port = port
        self.crypto = KdNetCrypto(key)
        self.event_callback = event_callback
        self.socket: socket.socket | None = None
        self.version_number = 1
        self.outer_send_sequence = 1
        self.inner_send_id = KD_INITIAL_ID ^ 1
        self.last_inner_sent: bytes | None = None
        self.last_received_signature: tuple[int, int, int] | None = None
        self.client_key: bytes | None = None
        self.host_key: bytes | None = None
        self.current_state: KdStateChange | None = None
        self.last_state_payload: bytes | None = None
        self.observed_modules: dict[int, KdModule] = {}
        self.version: KdVersion | None = None
        self.stopped = False
        self._manual_break_stop = False
        self._owns_log = False
        self._log: TextIO | None = None
        if isinstance(log, (str, Path)):
            self._log = Path(log).open("a", encoding="utf-8")
            self._owns_log = True
        elif log is not None:
            self._log = log

    def __enter__(self) -> "KdNetClient":
        return self

    def __exit__(self, *_args: object) -> None:
        self.close()

    def close(self) -> None:
        if self.socket is not None:
            self.socket.close()
            self.socket = None
        if self._owns_log and self._log is not None:
            self._log.close()
        self._log = None

    def _timestamp(self) -> str:
        return datetime.now(timezone.utc).isoformat(timespec="milliseconds")

    def _emit(self, kind: str, value: object) -> None:
        if self.event_callback:
            self.event_callback(kind, value)
        if self._log is None:
            return
        if kind == "debug-output":
            self._log.write(str(value))
        elif kind == "state" and isinstance(value, KdStateChange):
            self._log.write(
                f"\n[{self._timestamp()}] KD {value.kind}: "
                f"cpu={value.cpu}/{value.cpu_count} "
                f"pc=0x{value.program_counter:016x}"
            )
            if value.exception_code is not None:
                self._log.write(f" exception=0x{value.exception_code:08x}")
            if value.module_path:
                self._log.write(f" module={value.module_path}")
            self._log.write("\n")
        else:
            self._log.write(f"[{self._timestamp()}] {kind}: {value}\n")
        self._log.flush()

    def _open_socket(self) -> None:
        if self.socket is not None:
            return
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
        sock.bind((self.host, self.port))
        sock.connect((self.target, self.port))
        self.socket = sock

    def _send_outer(
        self,
        payload: bytes,
        packet_type: int,
        *,
        sequence: int | None = None,
    ) -> None:
        if self.socket is None:
            raise RuntimeError("KDNET socket is not open")
        if sequence is None:
            sequence = self.outer_send_sequence
            self.outer_send_sequence += 1
        packet = self.crypto.encode(
            payload,
            version=self.version_number,
            packet_type=packet_type,
            sequence=sequence,
            direction=KDNET_DIRECTION_DEBUGGER,
        )
        self.socket.send(packet)

    def _handle_poke(self, payload: bytes, version: int, sequence: int) -> None:
        if len(payload) != 34 or payload[:2] != b"\x01\x01":
            raise ProtocolError("invalid KDNET control poke")
        client_key = payload[2:]
        new_session = client_key != self.client_key
        if new_session:
            self.client_key = client_key
            self.host_key = secrets.token_bytes(32)
            self.outer_send_sequence = 1
            self.inner_send_id = KD_INITIAL_ID ^ 1
            self.last_inner_sent = None
            self.last_received_signature = None
            self.current_state = None
            self.last_state_payload = None
            self.observed_modules.clear()
            self.version = None
            self.stopped = False
            self._manual_break_stop = False
        assert self.host_key is not None
        self.version_number = version
        response = self.crypto.build_response(client_key, self.host_key)
        self._send_outer(response, KDNET_TYPE_CONTROL, sequence=sequence)
        self._emit("handshake", "new target session" if new_session else "poke retry")

    def _receive_inner(self, timeout: float) -> KdPacket:
        if self.socket is None:
            raise RuntimeError("KDNET socket is not open")
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise KdTimeout("timed out waiting for KDNET packet")
            self.socket.settimeout(min(remaining, 1.0))
            try:
                raw = self.socket.recv(65535)
            except socket.timeout:
                continue
            try:
                outer = self.crypto.decode(raw)
            except ProtocolError as error:
                self._emit("warning", str(error))
                continue
            if outer.direction != KDNET_DIRECTION_TARGET:
                continue
            if outer.packet_type == KDNET_TYPE_CONTROL:
                self._handle_poke(
                    outer.payload, outer.version, outer.sequence
                )
                continue
            if outer.packet_type != KDNET_TYPE_DATA:
                continue
            return decode_kd_packet(outer.payload)

    def _send_inner_data(self, packet_type: int, payload: bytes) -> int:
        self.inner_send_id ^= 1
        packet = encode_kd_data(packet_type, self.inner_send_id, payload)
        self.last_inner_sent = packet
        self._send_outer(packet, KDNET_TYPE_DATA)
        return self.inner_send_id

    def _send_inner_control(self, packet_type: int, packet_id: int) -> None:
        self._send_outer(
            encode_kd_control(packet_type, packet_id), KDNET_TYPE_DATA
        )

    def _acknowledge(self, packet: KdPacket) -> None:
        self._send_inner_control(
            KD_TYPE_ACKNOWLEDGE, packet.packet_id & ~KD_SYNC_ID
        )

    def _handle_debug_io(self, packet: KdPacket) -> None:
        if len(packet.payload) < 16:
            return
        api = struct.unpack_from("<I", packet.payload)[0]
        if api == DBGKD_PRINT_STRING_API:
            length = struct.unpack_from("<I", packet.payload, 8)[0]
            text = packet.payload[16:16 + length].decode("utf-8", errors="replace")
            self._emit("debug-output", text)
            return
        if api == DBGKD_GET_STRING_API:
            cpu_level, processor = struct.unpack_from("<HH", packet.payload, 4)
            prompt_length, maximum = struct.unpack_from("<II", packet.payload, 8)
            prompt = packet.payload[16:16 + prompt_length].decode(
                "utf-8", errors="replace"
            )
            self._emit("debug-output", prompt)

            # 'o' means break once and then ignore the assertion. This gives
            # the debugger a real exception stop without leaving the target
            # blocked forever in DbgPrompt.
            response = b"o"[:maximum]
            response_header = struct.pack(
                "<IHHII",
                DBGKD_GET_STRING_API,
                cpu_level,
                processor,
                0,
                len(response),
            )
            self._send_inner_data(KD_TYPE_DEBUG_IO, response_header + response)
            self._emit("debug-input", "replied 'o' (break once) to target prompt")
            return
        else:
            self._emit("debug-io", f"unsupported API 0x{api:x}")

    def _handle_file_io(self, _packet: KdPacket) -> None:
        response = struct.pack("<II", 0x3430, STATUS_NO_SUCH_FILE) + bytes(56)
        self._send_inner_data(KD_TYPE_FILE_IO, response)

    def _wait_for(
        self,
        expected_types: Iterable[int],
        timeout: float = 30.0,
    ) -> KdPacket:
        expected = set(expected_types)
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                names = ", ".join(str(value) for value in sorted(expected))
                raise KdTimeout(f"timed out waiting for inner KD type(s) {names}")
            packet = self._receive_inner(remaining)
            duplicate = False
            if packet.is_data:
                self._acknowledge(packet)
                signature = (
                    packet.packet_type,
                    packet.packet_id,
                    packet.checksum,
                )
                duplicate = signature == self.last_received_signature
                self.last_received_signature = signature
            if duplicate:
                continue
            if packet.packet_type == KD_TYPE_DEBUG_IO and packet.is_data:
                self._handle_debug_io(packet)
                continue
            if packet.packet_type == KD_TYPE_FILE_IO and packet.is_data:
                self._handle_file_io(packet)
                continue
            if (
                packet.packet_type == KD_TYPE_RESEND
                and packet.leader == KD_LEADER_CONTROL
            ):
                if self.last_inner_sent is None:
                    raise ProtocolError("target requested resend before any request")
                self._send_outer(self.last_inner_sent, KDNET_TYPE_DATA)
                continue
            if packet.packet_type in expected:
                return packet

    def connect(self, timeout: float = 300.0) -> KdStateChange:
        self._open_socket()
        deadline = time.monotonic() + timeout
        initial: KdPacket | None = None
        while initial is None:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise KdTimeout("target did not initiate KDNET")
            packet = self._receive_inner(remaining)
            if packet.packet_type == KD_TYPE_UNUSED:
                initial = packet
        self._send_outer(b"b", KDNET_TYPE_DATA)
        state_packet = self._wait_for(
            (KD_TYPE_STATE_CHANGE64,), max(1.0, deadline - time.monotonic())
        )
        self.inner_send_id = KD_INITIAL_ID ^ 1
        state = self._record_state(state_packet)
        self.stopped = True
        self.version = self.get_version()
        self._emit("version", self.version)
        return state

    def _record_state(self, packet: KdPacket) -> KdStateChange:
        state = parse_state_change(packet.payload)
        self.last_state_payload = packet.payload
        self.current_state = state
        if state.module_base is not None:
            if state.unloading:
                self.observed_modules.pop(state.module_base, None)
            else:
                path = state.module_path or ""
                self.observed_modules[state.module_base] = KdModule(
                    entry=0,
                    dll_base=state.module_base,
                    entry_point=0,
                    size=state.module_size or 0,
                    full_name=path,
                    base_name=path.replace("\\", "/").rsplit("/", 1)[-1],
                )
        self.stopped = True
        self._emit("state", state)
        return state

    def _build_manipulate(
        self,
        api: int,
        union: bytes = b"",
        *,
        processor: int | None = None,
    ) -> bytes:
        cpu = (
            processor
            if processor is not None
            else (self.current_state.cpu if self.current_state else 0)
        )
        if not 0 <= cpu <= 0xFFFF:
            raise ValueError("KD processor number does not fit in 16 bits")
        return (
            struct.pack("<IHHI", api, 0, cpu, 0)
            + bytes(4)
            + union.ljust(40, b"\0")[:40]
        )

    def _request(
        self,
        api: int,
        union: bytes = b"",
        data: bytes = b"",
        timeout: float = 30.0,
        processor: int | None = None,
    ) -> bytes:
        if not self.stopped:
            raise RuntimeError("KD manipulate requests require a stopped target")
        payload = self._build_manipulate(api, union, processor=processor) + data
        self._send_inner_data(KD_TYPE_STATE_MANIPULATE, payload)
        self._wait_for((KD_TYPE_ACKNOWLEDGE,), timeout)
        response = self._wait_for((KD_TYPE_STATE_MANIPULATE,), timeout)
        if len(response.payload) < KD_MANIPULATE_SIZE:
            raise ProtocolError("truncated KD manipulate response")
        response_api, status = struct.unpack_from("<IxxxxI", response.payload)
        if response_api != api:
            raise ProtocolError(
                f"KD response API 0x{response_api:x} does not match 0x{api:x}"
            )
        if status != STATUS_SUCCESS:
            raise KdRequestError(api, status)
        return response.payload

    def get_version(self) -> KdVersion:
        payload = self._request(DBGKD_GET_VERSION_API)
        union = 16
        major, build = struct.unpack_from("<HH", payload, union)
        protocol_major, protocol_minor = struct.unpack_from("<BB", payload, union + 4)
        flags, machine = struct.unpack_from("<HH", payload, union + 6)
        kernel_base, modules, debugger_data = struct.unpack_from(
            "<QQQ", payload, union + 16
        )
        return KdVersion(
            major,
            build,
            protocol_major,
            protocol_minor,
            flags,
            machine,
            kernel_base,
            modules,
            debugger_data,
        )

    def _prepare_manual_break_resume(self) -> None:
        """Advance RIP past an AMD64 break-in instruction when necessary.

        ReactOS reports a debugger-requested ``int3`` with ProgramCounter and
        CONTEXT.Rip pointing at the opcode, while ExceptionAddress points one
        byte past it.  Sending Continue without correcting the context executes
        the same ``int3`` again and returns to the debugger forever.  Restrict
        the correction to stops produced by :meth:`break_in`; ordinary target
        exceptions and debugger-planted breakpoints must retain their RIP.
        """
        if not self._manual_break_stop:
            return

        state = self.current_state
        if (
            state is None
            or state.state != DBGKD_EXCEPTION_STATE_CHANGE
            or state.exception_code != STATUS_BREAKPOINT
            or state.exception_address != state.program_counter + 1
        ):
            self._manual_break_stop = False
            return
        if self.version is not None and not self.version.is_64bit:
            self._manual_break_stop = False
            self._emit(
                "warning",
                "cannot adjust a rewound manual break on a non-AMD64 target",
            )
            return

        context = bytearray(self.get_context())
        rip_offset = 0xF8
        if len(context) < rip_offset + 8:
            raise ProtocolError("truncated AMD64 CONTEXT while resuming break-in")
        rip = struct.unpack_from("<Q", context, rip_offset)[0]
        if rip == state.program_counter:
            struct.pack_into("<Q", context, rip_offset, state.exception_address)
            self.set_context(bytes(context))
            self._emit(
                "resume-fixup",
                f"advanced manual break RIP from 0x{rip:016x} "
                f"to 0x{state.exception_address:016x}",
            )
        self._manual_break_stop = False

    def continue_execution(self) -> None:
        self._prepare_manual_break_resume()
        union = struct.pack("<II", DBG_CONTINUE, 0x400)
        payload = self._build_manipulate(DBGKD_CONTINUE_API, union)
        self._send_inner_data(KD_TYPE_STATE_MANIPULATE, payload)
        self._wait_for((KD_TYPE_ACKNOWLEDGE,))
        self.stopped = False

    def wait_for_stop(
        self,
        *,
        auto_continue_modules: bool = True,
        timeout: float = 86400.0,
    ) -> KdStateChange:
        deadline = time.monotonic() + timeout
        while True:
            packet = self._wait_for(
                (KD_TYPE_STATE_CHANGE64,),
                max(0.01, deadline - time.monotonic()),
            )
            state = self._record_state(packet)
            if (
                state.state == DBGKD_LOAD_SYMBOLS_STATE_CHANGE
                and auto_continue_modules
            ):
                self.continue_execution()
                continue
            return state

    def break_in(self, timeout: float = 30.0) -> KdStateChange:
        self._send_outer(b"b", KDNET_TYPE_DATA)
        state = self.wait_for_stop(timeout=timeout)
        self._manual_break_stop = True
        return state

    def read_virtual(self, address: int, size: int) -> bytes:
        return self._read_memory(DBGKD_READ_VIRTUAL_MEMORY_API, address, size)

    def read_physical(self, address: int, size: int) -> bytes:
        return self._read_memory(DBGKD_READ_PHYSICAL_MEMORY_API, address, size)

    def _read_memory(self, api: int, address: int, size: int) -> bytes:
        if size < 0:
            raise ValueError("memory size cannot be negative")
        result = bytearray()
        while len(result) < size:
            count = min(size - len(result), KD_TRANSFER_MAX)
            union = struct.pack("<QII", address + len(result), count, 0)
            response = self._request(api, union)
            actual = struct.unpack_from("<I", response, 16 + 12)[0]
            chunk = response[KD_MANIPULATE_SIZE:KD_MANIPULATE_SIZE + actual]
            result.extend(chunk)
            if not chunk or len(chunk) < count:
                break
        return bytes(result)

    def write_virtual(self, address: int, data: bytes) -> int:
        return self._write_memory(DBGKD_WRITE_VIRTUAL_MEMORY_API, address, data)

    def write_physical(self, address: int, data: bytes) -> int:
        return self._write_memory(DBGKD_WRITE_PHYSICAL_MEMORY_API, address, data)

    def _write_memory(self, api: int, address: int, data: bytes) -> int:
        written = 0
        maximum = KD_TRANSFER_MAX - KD_MANIPULATE_SIZE
        while written < len(data):
            chunk = data[written:written + maximum]
            union = struct.pack("<QII", address + written, len(chunk), 0)
            response = self._request(api, union, chunk)
            actual = struct.unpack_from("<I", response, 16 + 12)[0]
            written += actual
            if actual < len(chunk):
                break
        return written

    def get_context(self, processor: int | None = None) -> bytes:
        union = struct.pack("<I", CONTEXT_AMD64_ALL)
        response = self._request(
            DBGKD_GET_CONTEXT_API, union, processor=processor
        )
        return response[KD_MANIPULATE_SIZE:]

    def get_registers(self, processor: int | None = None) -> dict[str, int]:
        if self.version is not None and not self.version.is_64bit:
            raise NotImplementedError("i386 CONTEXT decoding is not implemented")
        return parse_amd64_context(self.get_context(processor))

    def set_context(self, context: bytes) -> None:
        union = struct.pack("<I", CONTEXT_AMD64_ALL)
        self._request(DBGKD_SET_CONTEXT_API, union, context)

    def set_breakpoint(self, address: int) -> int:
        response = self._request(
            DBGKD_WRITE_BREAKPOINT_API, struct.pack("<QI", address, 0)
        )
        return struct.unpack_from("<I", response, 16 + 8)[0]

    def clear_breakpoint(self, handle: int) -> None:
        self._request(DBGKD_RESTORE_BREAKPOINT_API, struct.pack("<I", handle))

    def query_memory(self, address: int) -> tuple[int, int]:
        union = struct.pack("<QQII", address, 0, 0, 0)
        response = self._request(DBGKD_QUERY_MEMORY_API, union)
        address_space, flags = struct.unpack_from("<II", response, 16 + 16)
        return address_space, flags

    def _read_unicode(self, address: int, length: int) -> str:
        if not address or not length:
            return ""
        if length > 0x10000 or length & 1:
            raise ProtocolError(f"invalid remote Unicode string length {length}")
        data = self.read_virtual(address, length)
        if len(data) != length:
            raise ProtocolError(
                f"short remote Unicode string read at 0x{address:016x}"
            )
        return data.decode("utf-16-le", errors="replace").rstrip("\0")

    def get_loaded_modules(self, max_modules: int = 512) -> list[KdModule]:
        """Walk PsLoadedModuleList using the AMD64 loader-entry layout."""
        if self.version is None or not self.version.is_64bit:
            raise NotImplementedError("module harvesting currently requires AMD64")
        if max_modules <= 0:
            raise ValueError("max_modules must be positive")
        head = self.version.loaded_module_list
        head_data = self.read_virtual(head, 16)
        if len(head_data) != 16:
            raise ProtocolError("could not read PsLoadedModuleList head")
        cursor = struct.unpack_from("<Q", head_data)[0]
        visited: set[int] = set()
        modules: list[KdModule] = []
        while cursor != head and len(modules) < max_modules:
            if not cursor or cursor in visited:
                raise ProtocolError("invalid or cyclic PsLoadedModuleList")
            visited.add(cursor)
            raw = self.read_virtual(cursor, 0x70)
            fields = parse_amd64_loader_entry(cursor, raw)
            modules.append(
                KdModule(
                    entry=cursor,
                    dll_base=fields["dll_base"],
                    entry_point=fields["entry_point"],
                    size=fields["size"],
                    full_name=self._read_unicode(
                        fields["full_name_buffer"], fields["full_name_length"]
                    ),
                    base_name=self._read_unicode(
                        fields["base_name_buffer"], fields["base_name_length"]
                    ),
                )
            )
            cursor = fields["flink"]
        if cursor != head:
            raise ProtocolError(
                f"PsLoadedModuleList exceeded the {max_modules}-entry limit"
            )
        return modules

    def get_debugger_data_blocks(
        self, max_blocks: int = 32, max_block_size: int = 0x100000
    ) -> list[KdDebuggerDataBlock]:
        """Walk DebuggerDataList and retrieve registered KDBG-style blocks."""
        if self.version is None or not self.version.is_64bit:
            raise NotImplementedError("debugger-data harvesting currently requires AMD64")
        head = self.version.debugger_data_list
        head_data = self.read_virtual(head, 16)
        if len(head_data) != 16:
            raise ProtocolError("could not read DebuggerDataList head")
        cursor = struct.unpack_from("<Q", head_data)[0]
        visited: set[int] = set()
        blocks: list[KdDebuggerDataBlock] = []
        while cursor != head and len(blocks) < max_blocks:
            if not cursor or cursor in visited:
                raise ProtocolError("invalid or cyclic DebuggerDataList")
            visited.add(cursor)
            header = self.read_virtual(cursor, 24)
            if len(header) != 24:
                raise ProtocolError("truncated debugger-data header")
            owner_raw, size = struct.unpack_from("<4sI", header, 16)
            if size < 24 or size > max_block_size:
                raise ProtocolError(
                    f"invalid debugger-data block size 0x{size:x} at 0x{cursor:016x}"
                )
            data = self.read_virtual(cursor, size)
            if len(data) != size:
                raise ProtocolError("truncated debugger-data block")
            blocks.append(
                KdDebuggerDataBlock(
                    address=cursor,
                    owner_tag=owner_raw.decode("ascii", errors="replace"),
                    size=size,
                    data=data,
                )
            )
            cursor = struct.unpack_from("<Q", header)[0]
        if cursor != head:
            raise ProtocolError(
                f"DebuggerDataList exceeded the {max_blocks}-entry limit"
            )
        return blocks

    def get_debug_print_log(
        self,
        blocks: list[KdDebuggerDataBlock] | None = None,
    ) -> tuple[bytes, bytes, dict[str, object]]:
        """Retrieve and order the target's standard KD DbgPrint ring."""
        if blocks is None:
            blocks = self.get_debugger_data_blocks()
        kdbg = next((block for block in blocks if block.owner_tag == "KDBG"), None)
        if kdbg is None:
            raise ProtocolError("target did not publish a KDBG debugger-data block")

        # KDDEBUGGER_DATA64 fields present since WS03.  The first pair points
        # at the actual ring and the second pair points at the live write and
        # rollover variables.
        ring_fields_offset = 0x1E0
        if len(kdbg.data) < ring_fields_offset + 32:
            raise ProtocolError("KDBG block is too old to describe the DbgPrint ring")
        ring_start, ring_end, write_pointer_address, rollover_address = (
            struct.unpack_from("<QQQQ", kdbg.data, ring_fields_offset)
        )
        ring_size = ring_end - ring_start
        if ring_start == 0 or ring_size <= 0 or ring_size > 0x1000000:
            raise ProtocolError(
                f"invalid DbgPrint ring 0x{ring_start:016x}-0x{ring_end:016x}"
            )

        pointer_data = self.read_virtual(write_pointer_address, 8)
        rollover_data = self.read_virtual(rollover_address, 4)
        if len(pointer_data) != 8 or len(rollover_data) != 4:
            raise ProtocolError("could not read DbgPrint ring state")
        write_pointer = struct.unpack("<Q", pointer_data)[0]
        rollover_count = struct.unpack("<I", rollover_data)[0]
        if not ring_start <= write_pointer <= ring_end:
            raise ProtocolError(
                f"DbgPrint write pointer 0x{write_pointer:016x} is outside its ring"
            )

        raw = self.read_virtual(ring_start, ring_size)
        if len(raw) != ring_size:
            raise ProtocolError(
                f"short DbgPrint ring read: expected {ring_size}, received {len(raw)}"
            )
        write_offset = write_pointer - ring_start
        if rollover_count == 0:
            ordered = raw[:write_offset]
        else:
            ordered = raw[write_offset:] + raw[:write_offset]

        metadata = {
            "ring_start": ring_start,
            "ring_end": ring_end,
            "ring_size": ring_size,
            "write_pointer": write_pointer,
            "write_offset": write_offset,
            "rollover_count": rollover_count,
        }
        return ordered, raw, metadata

    def harvest(
        self,
        directory: Path | str,
        *,
        stack_size: int = 0x4000,
        object_size: int = 0x1000,
        walk_modules: bool = False,
    ) -> Path:
        """Save a self-describing crash-style snapshot while the target is stopped."""
        if not self.stopped or self.current_state is None or self.version is None:
            raise RuntimeError("harvesting requires a stopped, initialized target")
        if stack_size <= 0 or object_size <= 0:
            raise ValueError("harvest sizes must be positive")
        destination = Path(directory)
        destination.mkdir(parents=True, exist_ok=True)
        manifest: dict[str, object] = {
            "created_utc": self._timestamp(),
            "state": asdict(self.current_state),
            "version": asdict(self.version),
            "regions": [],
            "errors": [],
        }
        regions = manifest["regions"]
        errors = manifest["errors"]
        assert isinstance(regions, list) and isinstance(errors, list)

        def save_manifest() -> None:
            temporary = destination / "manifest.json.tmp"
            temporary.write_text(
                json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
            )
            temporary.replace(destination / "manifest.json")

        def capture(name: str, address: int, size: int) -> bytes | None:
            try:
                data = self.read_virtual(address, size)
                if len(data) != size:
                    raise ProtocolError(
                        f"short read: requested {size}, received {len(data)}"
                    )
                filename = f"{name}.bin"
                (destination / filename).write_bytes(data)
                regions.append(
                    {"name": name, "address": address, "size": size, "file": filename}
                )
                save_manifest()
                return data
            except (KdRequestError, KdTimeout, ProtocolError, RuntimeError) as error:
                errors.append(
                    {"name": name, "address": address, "size": size, "error": str(error)}
                )
                save_manifest()
                return None

        def capture_progressive(
            name: str, address: int, size: int, chunk_size: int = 0x800
        ) -> bytes | None:
            """Persist each readable chunk and stop cleanly at an unmapped page."""
            filename = f"{name}.bin"
            data = bytearray()
            with (destination / filename).open("wb") as output:
                while len(data) < size:
                    cursor = address + len(data)
                    page_remaining = 0x1000 - (cursor & 0xFFF)
                    count = min(size - len(data), chunk_size, page_remaining)
                    try:
                        chunk = self.read_virtual(cursor, count)
                        if len(chunk) != count:
                            raise ProtocolError(
                                f"short read: requested {count}, received {len(chunk)}"
                            )
                    except (KdRequestError, KdTimeout, ProtocolError, RuntimeError) as error:
                        errors.append(
                            {
                                "name": name,
                                "address": cursor,
                                "requested_size": size,
                                "captured_size": len(data),
                                "error": str(error),
                            }
                        )
                        break
                    output.write(chunk)
                    output.flush()
                    data.extend(chunk)
                    regions[:] = [region for region in regions if region["name"] != name]
                    regions.append(
                        {
                            "name": name,
                            "address": address,
                            "size": len(data),
                            "requested_size": size,
                            "file": filename,
                        }
                    )
                    save_manifest()
            if not data:
                (destination / filename).unlink(missing_ok=True)
                save_manifest()
                return None
            return bytes(data)

        current_cpu = self.current_state.cpu
        processor_contexts: dict[str, object] = {}
        manifest["processor_contexts"] = processor_contexts

        try:
            context = self.get_context(current_cpu)
            (destination / "context-amd64.bin").write_bytes(context)
            registers = parse_amd64_context(context)
            manifest["registers"] = registers
            processor_contexts[str(current_cpu)] = {
                "file": "context-amd64.bin",
                "registers": registers,
            }
            save_manifest()
        except (KdRequestError, KdTimeout, ProtocolError, RuntimeError) as error:
            registers = {}
            errors.append({"name": "context-amd64", "error": str(error)})
            save_manifest()

        if self.last_state_payload is not None:
            (destination / "state-change.bin").write_bytes(self.last_state_payload)
            save_manifest()

        rip = registers.get("rip", self.current_state.program_counter)
        rsp = registers.get("rsp")
        capture("code", max(0, rip - 0x100), 0x300)
        stack_data = None
        processor_stacks: dict[int, tuple[int, bytes]] = {}
        if rsp is not None:
            stack_data = capture_progressive("stack-from-rsp", rsp, stack_size)
            if stack_data is not None:
                processor_stacks[current_cpu] = (rsp, stack_data)

        for processor in range(self.current_state.cpu_count):
            if processor == current_cpu:
                continue
            name = f"context-amd64-cpu{processor}"
            try:
                processor_context = self.get_context(processor)
                filename = f"{name}.bin"
                (destination / filename).write_bytes(processor_context)
                processor_registers = parse_amd64_context(processor_context)
                processor_contexts[str(processor)] = {
                    "file": filename,
                    "registers": processor_registers,
                }
                save_manifest()
                processor_rsp = processor_registers.get("rsp")
                if processor_rsp is not None:
                    processor_stack = capture_progressive(
                        f"stack-from-rsp-cpu{processor}",
                        processor_rsp,
                        stack_size,
                    )
                    if processor_stack is not None:
                        processor_stacks[processor] = (
                            processor_rsp,
                            processor_stack,
                        )
            except (KdRequestError, KdTimeout, ProtocolError, RuntimeError) as error:
                errors.append(
                    {"name": name, "processor": processor, "error": str(error)}
                )
                save_manifest()
        capture("current-thread", self.current_state.thread, object_size)
        capture("kernel-header", self.version.kernel_base, 0x1000)
        capture("loaded-module-list-head", self.version.loaded_module_list, 16)
        capture("debugger-data-list-head", self.version.debugger_data_list, 16)
        manifest["modules"] = [
            asdict(module)
            for module in sorted(
                self.observed_modules.values(), key=lambda value: value.dll_base
            )
        ]
        manifest["module_source"] = "observed KD load-symbol events"
        candidates_by_processor: dict[str, object] = {}
        modules = list(self.observed_modules.values())
        for processor, (processor_rsp, processor_stack) in processor_stacks.items():
            candidates = []
            for offset in range(0, len(processor_stack) - 7, 8):
                value = struct.unpack_from("<Q", processor_stack, offset)[0]
                for module in modules:
                    if module.dll_base <= value < module.dll_base + module.size:
                        candidates.append(
                            {
                                "stack_address": processor_rsp + offset,
                                "value": value,
                                "module": module.base_name or module.full_name,
                                "module_offset": value - module.dll_base,
                            }
                        )
                        break
            candidates_by_processor[str(processor)] = candidates
        manifest["stack_code_candidates_by_processor"] = candidates_by_processor
        manifest["stack_code_candidates"] = candidates_by_processor.get(
            str(current_cpu), []
        )
        save_manifest()

        if walk_modules:
            try:
                manifest["modules"] = [
                    asdict(module) for module in self.get_loaded_modules()
                ]
                manifest["module_source"] = "live PsLoadedModuleList walk"
            except (KdRequestError, KdTimeout, ProtocolError, RuntimeError, ValueError) as error:
                errors.append({"name": "loaded-modules", "error": str(error)})
            save_manifest()

        debugger_blocks: list[KdDebuggerDataBlock] = []
        try:
            block_metadata = []
            debugger_blocks = self.get_debugger_data_blocks()
            for index, block in enumerate(debugger_blocks):
                safe_tag = "".join(
                    character if character.isalnum() else "_"
                    for character in block.owner_tag
                )
                filename = f"debugger-data-{index:02d}-{safe_tag}.bin"
                (destination / filename).write_bytes(block.data)
                block_metadata.append(
                    {
                        "address": block.address,
                        "owner_tag": block.owner_tag,
                        "size": block.size,
                        "file": filename,
                    }
                )
            manifest["debugger_data_blocks"] = block_metadata
        except (KdRequestError, KdTimeout, ProtocolError, RuntimeError, ValueError) as error:
            errors.append({"name": "debugger-data", "error": str(error)})

        try:
            ordered_log, raw_ring, debug_print = self.get_debug_print_log(
                debugger_blocks or None
            )
            (destination / "dbgprint.log").write_bytes(ordered_log)
            (destination / "dbgprint-ring.bin").write_bytes(raw_ring)
            debug_print["file"] = "dbgprint.log"
            debug_print["raw_file"] = "dbgprint-ring.bin"
            manifest["debug_print"] = debug_print
        except (KdRequestError, KdTimeout, ProtocolError, RuntimeError, ValueError) as error:
            errors.append({"name": "debug-print-ring", "error": str(error)})

        save_manifest()
        self._emit("harvest", str(destination))
        return destination

    def status(self) -> dict[str, object]:
        return {
            "host": f"{self.host}:{self.port}",
            "target": f"{self.target}:{self.port}",
            "stopped": self.stopped,
            "state": asdict(self.current_state) if self.current_state else None,
            "version": asdict(self.version) if self.version else None,
        }


def console_event(kind: str, value: object) -> None:
    if kind == "debug-output":
        print(str(value), end="", flush=True)
    elif kind == "state" and isinstance(value, KdStateChange):
        detail = ""
        if value.exception_code is not None:
            detail = f" exception=0x{value.exception_code:08x}"
        if value.module_path:
            detail = f" module={value.module_path}"
        print(
            f"\n[KD] {value.kind} cpu={value.cpu}/{value.cpu_count} "
            f"pc=0x{value.program_counter:016x}{detail}",
            flush=True,
        )
    elif kind == "warning":
        print(f"[KD warning] {value}", file=sys.stderr, flush=True)
    elif kind == "handshake":
        print(f"[KDNET] {value}", file=sys.stderr, flush=True)
    elif kind == "debug-input":
        print(f"[KD] {value}", file=sys.stderr, flush=True)
