"""High-level ReactOS KDNET debugger session."""

from __future__ import annotations

import hashlib
import json
import os
import secrets
import socket
import struct
import sys
import time
from contextlib import contextmanager
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, Iterable, Iterator, TextIO

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
DBGKD_READ_IO_SPACE_API = 0x3139
DBGKD_WRITE_IO_SPACE_API = 0x313A
DBGKD_READ_PHYSICAL_MEMORY_API = 0x313D
DBGKD_WRITE_PHYSICAL_MEMORY_API = 0x313E
DBGKD_GET_VERSION_API = 0x3146
DBGKD_QUERY_MEMORY_API = 0x315C

DBG_CONTINUE = 0x00010002
STATUS_BREAKPOINT = 0x80000003
# The i386/AMD64 software breakpoint opcode (KD_BREAKPOINT_VALUE in the
# kernel).  A stop whose PC still sits on this byte has already executed it.
KD_BREAKPOINT_OPCODE = b"\xcc"
# Offsets of AMD64_DBGKD_CONTROL_REPORT fields inside a
# DBGKD_ANY_WAIT_STATE_CHANGE payload (verified against captured packets).
DBGKD_MAXSTREAM = 16
AMD64_INSTRUCTION_COUNT_OFFSET = 0xD4
AMD64_INSTRUCTION_STREAM_OFFSET = 0xD8
STATUS_SUCCESS = 0
STATUS_NO_SUCH_FILE = 0xC000000F
KD_MANIPULATE_SIZE = 56
KD_FILE_IO_SIZE = 64
KD_TRANSFER_MAX = 0x800
CONTEXT_AMD64_ALL = 0x0010003F

# AMD64 kernel/user structure offsets for THIS build (NTDDI 0xa000000, NT 10 dual).
# Derived with an offsetof() probe compiled against build_nt10_dual_cmake's NDK
# headers with the same defines as the genincdata target. The probe's anchor
# fields reproduced ksamd64.inc exactly (EpDebugPort=0x170, EpWow64Process=0x298,
# KTHREAD.ApcState=0x98, KAPC_STATE.Process=0x20, KTHREAD.Process=0x220), and the
# +0xB8 (ApcState.Process) path was cross-checked against a live ETHREAD page.
KTHREAD_APCSTATE = 0x98            # KTHREAD.ApcState (ThApcState)
KAPC_STATE_PROCESS = 0x20          # KAPC_STATE.Process (AsProcess)
KTHREAD_APCSTATE_PROCESS = KTHREAD_APCSTATE + KAPC_STATE_PROCESS  # 0xB8 -> EPROCESS*
EPROCESS_UNIQUE_PROCESS_ID = 0xF0
EPROCESS_IMAGE_FILE_NAME = 0x258   # CHAR ImageFileName[16]
EPROCESS_IMAGE_FILE_NAME_LEN = 16
EPROCESS_PEB = 0x2B0               # struct _PEB *Peb
PEB_LDR = 0x18                     # PEB.Ldr (PPEB_LDR_DATA)
PEB_LDR_DATA_INLOADORDER = 0x10    # PEB_LDR_DATA.InLoadOrderModuleList (LIST_ENTRY)
LDTE_INLOADORDER_LINKS = 0x00      # LDR_DATA_TABLE_ENTRY.InLoadOrderLinks (list head)
LDTE_DLL_BASE = 0x30
LDTE_ENTRY_POINT = 0x38
LDTE_SIZE_OF_IMAGE = 0x40
LDTE_FULL_DLL_NAME = 0x48          # UNICODE_STRING
LDTE_BASE_DLL_NAME = 0x58          # UNICODE_STRING
PAGE_SIZE = 0x1000

# --- KDDEBUGGER_DATA64 field offsets ---------------------------------------
# Fixed by the on-the-wire KDBG layout (sdk/include/psdk/wdbgexts.h), not by
# the build: every field up to RetpolineStubSize is a naturally aligned
# ULONG64/ULPTR64/USHORT.  Anchored against live captures -- KernBase (+0x18)
# and PsLoadedModuleList (+0x48) reproduce KdVersion.kernel_base and
# .loaded_module_list exactly in every harvest under the ASUS capture set.
KDBG_KERN_BASE = 0x18
KDBG_PS_LOADED_MODULE_LIST = 0x48
KDBG_KI_PROCESSOR_BLOCK = 0x218    # PKPRCB KiProcessorBlock[MAXIMUM_PROCESSORS]
KDBG_SIZE_PRCB = 0x2B0             # USHORT sizeof(KPRCB)
KDBG_OFFSET_PRCB_CURRENT_THREAD = 0x2B4
KDBG_OFFSET_PRCB_NUMBER = 0x2BE
# Reading Number needs everything up to and including that field.
KDBG_PRCB_METRICS_MINIMUM = KDBG_OFFSET_PRCB_NUMBER + 2

# --- KPRCB (AMD64, NTDDI 0xA000000) ----------------------------------------
# ReactOS' KPRCB is *not* the Windows 10 KPRCB: it is the WS03/Vista-shaped
# struct in sdk/include/ndk/amd64/ketypes.h with NTDDI_VERSION >= NTDDI_LONGHORN
# branches taken.  IpiFrozen therefore sits at 0x2218, nowhere near where a
# Windows 10 symbol file would put it, so it cannot be looked up from a PDB and
# must come from this tree.  Value taken from the generated
# build_nt10_dual_cmake/sdk/include/asm/ksamd64.inc (PbIpiFrozen = 0x2218,
# PbTargetSet = 0x2210, ProcessorBlockLength = 0x42a0), which is emitted by the
# genincdata target compiled with the exact defines of the target kernel.
# Cross-checked at run time: the KDBG the target publishes carries SizePrcb,
# OffsetPrcbNumber and OffsetPrcbCurrentThread, and a mismatch against these
# constants is recorded in the manifest instead of silently trusted.
PRCB_NUMBER = 0x04                 # USHORT Number   (NT6+ layout; UCHAR on WS03)
PRCB_CURRENT_THREAD = 0x08         # PKTHREAD CurrentThread
PRCB_TARGET_SET = 0x2210           # UINT64 TargetSet
PRCB_IPI_FROZEN = 0x2218           # ULONG IpiFrozen
PRCB_SIZE = 0x42A0                 # sizeof(KPRCB)
# The two slices the freeze-state capture reads out of each PRCB.  Kept small
# on purpose: a KD virtual read is 0x800 bytes per round trip, and a stalled
# target is exactly when we cannot afford 0x42A0 bytes times every core.
PRCB_HEAD_SIZE = 0x10              # MxCsr, Number, InterruptRequest, IdleHalt,
                                   # CurrentThread
PRCB_IPI_SLICE_SIZE = 0x10         # TargetSet + IpiFrozen (+ PrcbPad3 head)

# KPRCB::IpiFrozen states, from sdk/include/ndk/amd64/ketypes.h.  The low
# nibble is the state; 0x20 is the "active in the debugger" flag.
IPI_FROZEN_FLAG_ACTIVE = 0x20
IPI_FROZEN_STATE_RUNNING = 0x0
IPI_FROZEN_STATE_FROZEN = 0x2
IPI_FROZEN_STATE_THAW = 0x3
IPI_FROZEN_STATE_OWNER = 0x4
IPI_FROZEN_STATE_TARGET_FREEZE = 0x5
IPI_FROZEN_STATE_NAMES = {
    IPI_FROZEN_STATE_RUNNING: "running",
    IPI_FROZEN_STATE_FROZEN: "frozen",
    IPI_FROZEN_STATE_THAW: "thaw",
    IPI_FROZEN_STATE_OWNER: "freeze-owner",
    IPI_FROZEN_STATE_TARGET_FREEZE: "target-freeze",
}
# Highest 4-level canonical kernel address space start on AMD64.  A PRCB
# pointer below this is not a kernel pointer and must not be dereferenced.
AMD64_KERNEL_SPACE_START = 0xFFFF800000000000
# MAXIMUM_PROCESSORS on this build; also the KiProcessorBlock array bound.
KI_PROCESSOR_BLOCK_MAXIMUM = 64


class KdTimeout(TimeoutError):
    """The target did not produce the expected KD packet in time."""


class KdSessionChanged(Exception):
    """A new KDNET target session replaced the active one."""


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
    # Hex of the bytes at ProgramCounter reported by an AMD64 target, kept as a
    # string so a state change stays JSON-serialisable into harvest manifests.
    amd64_instruction_stream: str = ""

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
        # AMD64_DBGKD_CONTROL_REPORT follows the fixed-size exception union at
        # 0xC0: Dr6, Dr7, EFlags, InstructionCount, ReportFlags,
        # InstructionStream[16].  It carries the bytes at ProgramCounter, so the
        # resume path can recognise an executed int3 without a memory read.
        # Only meaningful for an AMD64 target; callers must check that first.
        if len(payload) >= AMD64_INSTRUCTION_STREAM_OFFSET:
            count = struct.unpack_from(
                "<H", payload, AMD64_INSTRUCTION_COUNT_OFFSET
            )[0]
            count = min(count, DBGKD_MAXSTREAM)
            values["amd64_instruction_stream"] = payload[
                AMD64_INSTRUCTION_STREAM_OFFSET:
                AMD64_INSTRUCTION_STREAM_OFFSET + count
            ].hex()
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


def _summarise_freeze_state(prcbs: dict[str, object]) -> str:
    """One-line, human-readable digest of a get_processor_control_blocks() result."""
    processors = prcbs.get("processors") or []
    parts = []
    for entry in processors:
        index = entry.get("processor")
        if "state" in entry:
            flag = "+active" if entry.get("active") else ""
            parts.append(f"cpu{index}={entry['state']}{flag}")
        else:
            parts.append(f"cpu{index}=?({entry.get('error', 'no data')})")
    owner = prcbs.get("freeze_owner_processor")
    summary = " ".join(parts) if parts else "no processors read"
    summary += f" owner={'cpu%d' % owner if owner is not None else 'unknown'}"
    if prcbs.get("layout_warnings"):
        summary += " LAYOUT-MISMATCH:" + ";".join(prcbs["layout_warnings"])
    if prcbs.get("error"):
        summary += f" error={prcbs['error']}"
    return summary


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
        session_state: Path | str | None = None,
    ):
        self.host = host
        self.target = target
        self.port = port
        self.crypto = KdNetCrypto(key)
        self.event_callback = event_callback
        self._session_state_path = (
            Path(session_state) if session_state is not None else None
        )
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
        self._planted_breakpoints: dict[int, int] = {}
        self._operation_deadline: float | None = None
        self._request_in_flight = False
        self.last_harvest_complete: bool | None = None
        self._owns_log = False
        self._log: TextIO | None = None
        if isinstance(log, (str, Path)):
            self._log = Path(log).open("a", encoding="utf-8")
            self._owns_log = True
        elif log is not None:
            self._log = log
        self._load_session_state()

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

    def _load_session_state(self) -> None:
        path = self._session_state_path
        if path is None or not path.exists():
            return
        try:
            saved = json.loads(path.read_text(encoding="utf-8"))
            if saved.get("format") != 1:
                raise ValueError("unsupported format")
            client_key = bytes.fromhex(saved["client_key"])
            host_key = bytes.fromhex(saved["host_key"])
            version = saved["version"]
            if len(client_key) != 32 or len(host_key) != 32:
                raise ValueError("keys must be 32 bytes")
            if isinstance(version, bool) or not isinstance(version, int):
                raise ValueError("version must be an integer")
            if not 0 <= version <= 0xFF:
                raise ValueError("version is outside the byte range")
            self.crypto.build_response(client_key, host_key)
        except (KeyError, OSError, TypeError, ValueError, ProtocolError) as error:
            self._emit("warning", f"ignored KDNET session state {path}: {error}")
            return

        self.client_key = client_key
        self.host_key = host_key
        self.version_number = version
        fingerprint = hashlib.sha256(client_key).hexdigest()
        self._emit(
            "session-state",
            f"restored client_key_sha256={fingerprint}",
        )

    def _persist_session_state(self) -> None:
        path = getattr(self, "_session_state_path", None)
        if path is None or self.client_key is None or self.host_key is None:
            return

        saved = {
            "format": 1,
            "version": self.version_number,
            "client_key": self.client_key.hex(),
            "host_key": self.host_key.hex(),
        }
        temporary = path.with_name(f".{path.name}.tmp-{os.getpid()}")
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC
            if hasattr(os, "O_NOFOLLOW"):
                flags |= os.O_NOFOLLOW
            descriptor = os.open(temporary, flags, 0o600)
            try:
                os.fchmod(descriptor, 0o600)
                with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
                    descriptor = -1
                    stream.write(json.dumps(saved, sort_keys=True) + "\n")
                    stream.flush()
                    os.fsync(stream.fileno())
            finally:
                if descriptor >= 0:
                    os.close(descriptor)
            os.replace(temporary, path)
            os.chmod(path, 0o600)
        except OSError as error:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass
            except OSError:
                pass
            self._emit("warning", f"could not save KDNET session state {path}: {error}")

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
        client_key_sha256 = hashlib.sha256(client_key).hexdigest()
        had_session = self.client_key is not None
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
            self._planted_breakpoints.clear()
            self._request_in_flight = False
            self.last_harvest_complete = None
        assert self.host_key is not None
        self.version_number = version
        response = self.crypto.build_response(client_key, self.host_key)
        self._persist_session_state()
        self._send_outer(response, KDNET_TYPE_CONTROL, sequence=sequence)
        handshake = "new target session" if new_session else "poke retry"
        handshake += f" client_key_sha256={client_key_sha256}"
        if new_session and had_session:
            self._emit("session-change", "new target replaced active KDNET session")
            self._emit("handshake", handshake)
            raise KdSessionChanged("new target replaced active KDNET session")
        self._emit("handshake", handshake)

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

    @contextmanager
    def operation_deadline(self, deadline: float | None) -> Iterator[None]:
        previous = getattr(self, "_operation_deadline", None)
        if deadline is None or previous is None:
            self._operation_deadline = deadline if deadline is not None else previous
        else:
            self._operation_deadline = min(previous, deadline)
        try:
            yield
        finally:
            self._operation_deadline = previous

    def _wait_for(
        self,
        expected_types: Iterable[int],
        timeout: float = 30.0,
    ) -> KdPacket:
        expected = set(expected_types)
        deadline = time.monotonic() + timeout
        operation_deadline = getattr(self, "_operation_deadline", None)
        if operation_deadline is not None:
            deadline = min(deadline, operation_deadline)
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
        resuming = (
            self.client_key is not None
            and self.host_key is not None
            and self.crypto.data_key is not None
        )
        if resuming:
            fingerprint = hashlib.sha256(self.client_key).hexdigest()
            self._emit(
                "handshake",
                f"resuming target session client_key_sha256={fingerprint}",
            )
            self._send_outer(b"b", KDNET_TYPE_DATA)
            first = self._wait_for(
                (KD_TYPE_UNUSED, KD_TYPE_STATE_CHANGE64),
                max(1.0, deadline - time.monotonic()),
            )
            if first.packet_type == KD_TYPE_STATE_CHANGE64:
                state_packet = first
            else:
                self._send_outer(b"b", KDNET_TYPE_DATA)
                state_packet = self._wait_for(
                    (KD_TYPE_STATE_CHANGE64,),
                    max(1.0, deadline - time.monotonic()),
                )
        else:
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
                (KD_TYPE_STATE_CHANGE64,),
                max(1.0, deadline - time.monotonic()),
            )
        self.inner_send_id = KD_INITIAL_ID ^ 1
        state = self._record_state(state_packet)
        self.stopped = True
        # Both fresh and persisted connects stop the target with a raw KD break
        # byte. AMD64 reports RIP on the synthetic int3 and must advance it once
        # before the first Continue, just like break_in().
        self._manual_break_stop = True
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
        if self._request_in_flight:
            raise RuntimeError("a previous KD manipulate response is still pending")
        payload = self._build_manipulate(api, union, processor=processor) + data
        self._request_in_flight = True
        try:
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
        except KdTimeout:
            # A late response may still arrive.  The caller must drain it before
            # issuing an unrelated manipulate request such as Continue.
            raise
        except Exception:
            self._request_in_flight = False
            raise
        self._request_in_flight = False
        return response.payload

    def drain_pending_request(self, timeout: float = 5.0) -> None:
        if not getattr(self, "_request_in_flight", False):
            return
        self._wait_for((KD_TYPE_STATE_MANIPULATE,), timeout)
        self._request_in_flight = False

    def poll_idle(self, timeout: float = 0.5) -> None:
        """Service the KDNET socket while no request is outstanding.

        A target announces a reboot with a KDNET poke, and KdSessionChanged is
        raised from handling that poke -- which only happens inside a receive.
        So any caller that waits on something *other* than the socket, such as
        a REPL waiting for its next command, must call this while it waits.
        Otherwise a target that reboots during the wait is invisible: nothing
        reads the socket, the reconnect never runs, and the handshake packets
        simply accumulate in the receive buffer.

        Waits for a packet type that can never arrive, so every packet is
        dispatched the usual way -- pokes raise, debug and file I/O are logged
        -- and nothing is treated as a reply.
        """
        try:
            self._wait_for((), timeout)
        except KdTimeout:
            pass

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
        """Step over an already-executed ``int3`` before resuming the target.

        The kernel reports a software breakpoint with CONTEXT.Rip pointing *at*
        the ``0xCC`` opcode: ``KiDispatchException`` rewinds Rip by one
        (ntoskrnl/ke/amd64/except.c) and KD64 never puts it back for a
        BREAKPOINT_BREAK (ntoskrnl/kd64/kdtrap.c only bumps the PC for the
        int-2Dh debug services).  That is Windows behaviour -- the
        ntdll:exception ``int3_handler`` asserts that both ExceptionAddress and
        Rip equal the address of the int3 and does ``context->Rip++`` itself --
        so stepping over the opcode is the *debugger's* job.  Omit it and the
        very same int3 re-executes immediately after every Continue, which on
        the wire looks exactly like a target that refuses to resume.

        Decide on the opcode actually present at the reported PC, not on any
        ExceptionAddress/ProgramCounter relationship.  Kernels built before
        ntoskrnl/ke/amd64/trap.S grew its BREAKPOINT_BREAK adjustment reported
        ExceptionAddress one byte past the PC; current ones report the two
        equal.  Both need the identical correction, and only the opcode
        distinguishes a breakpoint that ran from a STATUS_BREAKPOINT raised by
        software.  Breakpoints this client planted are left alone: their
        ``0xCC`` hides a real instruction byte and has to be removed, never
        stepped over.
        """
        manual = self._manual_break_stop
        self._manual_break_stop = False

        if not self.stopped:
            return

        state = self.current_state
        if (
            state is None
            or state.state != DBGKD_EXCEPTION_STATE_CHANGE
            or state.exception_code != STATUS_BREAKPOINT
        ):
            return

        if self.version is not None and not self.version.is_64bit:
            if manual:
                self._emit(
                    "warning",
                    "cannot adjust a rewound manual break on a non-AMD64 target",
                )
            return

        pc = state.program_counter
        if pc in self._planted_breakpoints.values():
            return

        opcode = bytes.fromhex(state.amd64_instruction_stream[:2])
        if not opcode:
            try:
                opcode = self.read_virtual(pc, 1)
            except (KdRequestError, KdTimeout, ProtocolError):
                opcode = b""
        if opcode[:1] != KD_BREAKPOINT_OPCODE:
            if opcode:
                # A real instruction byte: the breakpoint status was raised by
                # software (RtlRaiseException) and the PC must not move.
                return
            # The opcode could not be read back.  Fall back to the signature of
            # the older kernels, which reported ExceptionAddress == PC + 1 for
            # an executed int3.
            if state.exception_address != pc + 1:
                self._emit(
                    "warning",
                    f"cannot read the opcode at 0x{pc:016x}; resuming without "
                    "stepping over a possible breakpoint",
                )
                return

        context = bytearray(self.get_context())
        rip_offset = 0xF8
        if len(context) < rip_offset + 8:
            raise ProtocolError("truncated AMD64 CONTEXT while resuming break-in")
        rip = struct.unpack_from("<Q", context, rip_offset)[0]
        if rip != pc:
            # Somebody already moved the PC off the breakpoint.
            return
        struct.pack_into("<Q", context, rip_offset, pc + 1)
        self.set_context(bytes(context))
        self._emit(
            "resume-fixup",
            f"stepped over int3 at 0x{pc:016x}: "
            f"RIP 0x{rip:016x} -> 0x{pc + 1:016x}",
        )

    def continue_execution(self) -> None:
        self.drain_pending_request()
        self._prepare_manual_break_resume()
        union = struct.pack("<II", DBG_CONTINUE, 0x400)
        payload = self._build_manipulate(DBGKD_CONTINUE_API, union)
        self._send_inner_data(KD_TYPE_STATE_MANIPULATE, payload)
        # Once Continue is on the wire, a missing ACK is ambiguous: the target
        # may already be running.  Never issue a duplicate Continue on retry.
        self.stopped = False
        self._wait_for((KD_TYPE_ACKNOWLEDGE,))

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

    @staticmethod
    def _validate_io_size(size: int) -> None:
        if size not in (1, 2, 4):
            raise ValueError("I/O size must be 1, 2, or 4 bytes")

    def read_io(self, address: int, size: int) -> int:
        self._validate_io_size(size)
        response = self._request(
            DBGKD_READ_IO_SPACE_API,
            struct.pack("<QII", address, size, 0),
        )
        _, actual_size, value = struct.unpack_from("<QII", response, 16)
        if actual_size != size:
            raise ProtocolError(
                f"short I/O read at 0x{address:x}: requested {size}, "
                f"received {actual_size}"
            )
        return value & ((1 << (size * 8)) - 1)

    def write_io(self, address: int, size: int, value: int) -> int:
        self._validate_io_size(size)
        if value < 0 or value >= 1 << (size * 8):
            raise ValueError(f"I/O value does not fit in {size} byte(s)")
        response = self._request(
            DBGKD_WRITE_IO_SPACE_API,
            struct.pack("<QII", address, size, value),
        )
        _, actual_size, _ = struct.unpack_from("<QII", response, 16)
        return actual_size

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
        handle = struct.unpack_from("<I", response, 16 + 8)[0]
        # Remember where we planted it: the 0xCC there hides a real instruction
        # byte, so the resume path must not step over it like an executed one.
        self._planted_breakpoints[handle] = address
        return handle

    def clear_breakpoint(self, handle: int) -> None:
        self._request(DBGKD_RESTORE_BREAKPOINT_API, struct.pack("<I", handle))
        self._planted_breakpoints.pop(handle, None)

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

    def read_pointer(self, address: int) -> int:
        """Read a single little-endian 64-bit pointer from the target."""
        data = self.read_virtual(address, 8)
        if len(data) != 8:
            raise ProtocolError(f"short pointer read at 0x{address:016x}")
        return struct.unpack_from("<Q", data)[0]

    def _read_unicode_string(self, address: int) -> str:
        """Read a UNICODE_STRING {Length, MaximumLength, Buffer} and its buffer."""
        header = self.read_virtual(address, 0x10)
        if len(header) != 0x10:
            raise ProtocolError(f"short UNICODE_STRING read at 0x{address:016x}")
        length = struct.unpack_from("<H", header, 0)[0]
        buffer = struct.unpack_from("<Q", header, 8)[0]
        return self._read_unicode(buffer, length)

    def get_current_process(self) -> dict[str, object]:
        """Resolve the current thread's EPROCESS and its identifying fields.

        Uses KTHREAD.ApcState.Process (ETHREAD+0xB8) which holds the KPROCESS /
        EPROCESS base of the process the current thread is attached to.
        """
        if self.current_state is None:
            raise RuntimeError("no current state")
        thread = self.current_state.thread
        eprocess = self.read_pointer(thread + KTHREAD_APCSTATE_PROCESS)
        info: dict[str, object] = {"eprocess": eprocess}
        name = self.read_virtual(
            eprocess + EPROCESS_IMAGE_FILE_NAME, EPROCESS_IMAGE_FILE_NAME_LEN
        )
        info["image_file_name"] = name.split(b"\0", 1)[0].decode(
            "latin-1", errors="replace"
        )
        info["image_file_name_raw"] = name.hex()
        try:
            info["unique_process_id"] = self.read_pointer(
                eprocess + EPROCESS_UNIQUE_PROCESS_ID
            )
        except (KdRequestError, KdTimeout, ProtocolError) as error:
            info["unique_process_id_error"] = str(error)
        try:
            info["peb"] = self.read_pointer(eprocess + EPROCESS_PEB)
        except (KdRequestError, KdTimeout, ProtocolError) as error:
            info["peb_error"] = str(error)
        return info

    def get_user_modules(
        self, peb: int, max_modules: int = 1024
    ) -> list[dict[str, object]]:
        """Walk the user PEB->Ldr->InLoadOrderModuleList while in-process.

        All user VAs are readable while the target is stopped in this process's
        context, so the LDR_DATA_TABLE_ENTRY chain and its UNICODE_STRING buffers
        can be read directly.
        """
        if not peb:
            raise ProtocolError("process has no PEB (kernel/system thread?)")
        ldr = self.read_pointer(peb + PEB_LDR)
        if not ldr:
            raise ProtocolError("PEB.Ldr is NULL (loader not yet initialized?)")
        head = ldr + PEB_LDR_DATA_INLOADORDER
        cursor = self.read_pointer(head)
        visited: set[int] = set()
        modules: list[dict[str, object]] = []
        while cursor != head and len(modules) < max_modules:
            if not cursor or cursor in visited:
                break
            visited.add(cursor)
            entry = cursor - LDTE_INLOADORDER_LINKS
            try:
                base = self.read_pointer(entry + LDTE_DLL_BASE)
                size = struct.unpack_from(
                    "<I", self.read_virtual(entry + LDTE_SIZE_OF_IMAGE, 4)
                )[0]
                entry_point = self.read_pointer(entry + LDTE_ENTRY_POINT)
                full_name = self._read_unicode_string(entry + LDTE_FULL_DLL_NAME)
                base_name = self._read_unicode_string(entry + LDTE_BASE_DLL_NAME)
                modules.append(
                    {
                        "entry": entry,
                        "dll_base": base,
                        "entry_point": entry_point,
                        "size": size,
                        "full_name": full_name,
                        "base_name": base_name,
                    }
                )
            except (KdRequestError, KdTimeout, ProtocolError) as error:
                modules.append({"entry": entry, "error": str(error)})
            cursor = self.read_pointer(cursor + LDTE_INLOADORDER_LINKS)
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

    @staticmethod
    def describe_ipi_frozen(value: int) -> dict[str, object]:
        """Decode a KPRCB::IpiFrozen word into the !ipi-style freeze state."""
        state = value & ~IPI_FROZEN_FLAG_ACTIVE
        return {
            "ipi_frozen": value,
            "state": IPI_FROZEN_STATE_NAMES.get(state, f"unknown-0x{state:x}"),
            "active": bool(value & IPI_FROZEN_FLAG_ACTIVE),
            # Actually parked in KiProcessorFreezeHandler.  TARGET_FREEZE means
            # the freeze was *requested* and the NMI has not landed yet, which
            # is the interesting failure: an AP stuck at target-freeze never
            # froze at all.
            "is_frozen": state in (IPI_FROZEN_STATE_FROZEN, IPI_FROZEN_STATE_THAW),
            "is_freeze_owner": state == IPI_FROZEN_STATE_OWNER,
            "freeze_requested": state == IPI_FROZEN_STATE_TARGET_FREEZE,
        }

    def get_processor_control_blocks(
        self,
        blocks: list[KdDebuggerDataBlock] | None = None,
        processor_count: int | None = None,
    ) -> dict[str, object]:
        """Read each CPU's KPRCB freeze state through KDBG's KiProcessorBlock.

        Answers the two questions a stalled SMP target raises and that a
        context dump cannot: is this core actually frozen, and which core owns
        the freeze.  KxFreezeExecution stamps IPI_FROZEN_STATE_OWNER into the
        winning CPU's PRCB immediately after it takes KiFreezeOwner, so the
        owner is recoverable from the PRCBs alone -- KiFreezeOwner itself is a
        plain kernel global that KDBG does not publish.

        Never raises for a single unreadable or implausible PRCB: those are
        recorded per processor and the walk continues.  KdTimeout is allowed
        out so the caller's capture deadline still applies.
        """
        if self.version is None or not self.version.is_64bit:
            raise NotImplementedError("PRCB harvesting currently requires AMD64")
        if blocks is None:
            blocks = self.get_debugger_data_blocks()
        kdbg = next((block for block in blocks if block.owner_tag == "KDBG"), None)
        if kdbg is None:
            raise ProtocolError("target did not publish a KDBG debugger-data block")
        if len(kdbg.data) < KDBG_PRCB_METRICS_MINIMUM:
            raise ProtocolError(
                f"KDBG block is only 0x{len(kdbg.data):x} bytes, too old to "
                "describe KiProcessorBlock"
            )

        array = struct.unpack_from("<Q", kdbg.data, KDBG_KI_PROCESSOR_BLOCK)[0]
        size_prcb = struct.unpack_from("<H", kdbg.data, KDBG_SIZE_PRCB)[0]
        offset_number = struct.unpack_from("<H", kdbg.data, KDBG_OFFSET_PRCB_NUMBER)[0]
        offset_thread = struct.unpack_from(
            "<H", kdbg.data, KDBG_OFFSET_PRCB_CURRENT_THREAD
        )[0]

        # The target tells us three KPRCB offsets.  If any disagrees with the
        # constants above then this kernel is not the one they were generated
        # from and PRCB_IPI_FROZEN is a guess -- say so in the manifest rather
        # than publish a freeze state nobody can trust.
        layout_warnings: list[str] = []
        for name, reported, expected in (
            ("SizePrcb", size_prcb, PRCB_SIZE),
            ("OffsetPrcbNumber", offset_number, PRCB_NUMBER),
            ("OffsetPrcbCurrentThread", offset_thread, PRCB_CURRENT_THREAD),
        ):
            if reported != expected:
                layout_warnings.append(
                    f"{name}=0x{reported:x} but this build expects 0x{expected:x}"
                )

        if processor_count is None:
            processor_count = (
                self.current_state.cpu_count if self.current_state else 1
            )
        processor_count = max(0, min(processor_count, KI_PROCESSOR_BLOCK_MAXIMUM))

        result: dict[str, object] = {
            "processor_block_array": array,
            "size_prcb": size_prcb,
            "offset_prcb_number": offset_number,
            "offset_prcb_current_thread": offset_thread,
            "offset_prcb_ipi_frozen": PRCB_IPI_FROZEN,
            "ipi_frozen_offset_source": (
                "build_nt10_dual_cmake/sdk/include/asm/ksamd64.inc PbIpiFrozen"
            ),
            "layout_warnings": layout_warnings,
            "processors": [],
            "freeze_owner_processor": None,
        }
        processors: list[dict[str, object]] = result["processors"]
        if array < AMD64_KERNEL_SPACE_START or array % 8:
            result["error"] = (
                f"KiProcessorBlock 0x{array:016x} is not a kernel pointer"
            )
            return result

        pointer_bytes = self.read_virtual(array, processor_count * 8)
        available = len(pointer_bytes) // 8
        if available < processor_count:
            result["error"] = (
                f"short KiProcessorBlock read: wanted {processor_count} entries, "
                f"received {available}"
            )
        pointers = list(
            struct.unpack_from(f"<{available}Q", pointer_bytes)
        ) if available else []

        for index, prcb in enumerate(pointers):
            entry: dict[str, object] = {"processor": index, "prcb": prcb}
            processors.append(entry)
            if prcb == 0:
                entry["error"] = "KiProcessorBlock entry is NULL"
                continue
            if prcb < AMD64_KERNEL_SPACE_START or prcb % 16:
                entry["error"] = f"implausible PRCB pointer 0x{prcb:016x}"
                continue
            try:
                head = self.read_virtual(prcb, PRCB_HEAD_SIZE)
                if len(head) < PRCB_HEAD_SIZE:
                    raise ProtocolError(
                        f"short PRCB read at 0x{prcb:016x}: {len(head)} bytes"
                    )
                slice_address = prcb + PRCB_TARGET_SET
                ipi = self.read_virtual(slice_address, PRCB_IPI_SLICE_SIZE)
                if len(ipi) < PRCB_IPI_FROZEN - PRCB_TARGET_SET + 4:
                    raise ProtocolError(
                        f"short PRCB freeze-state read at 0x{slice_address:016x}: "
                        f"{len(ipi)} bytes"
                    )
            # KdTimeout derives from TimeoutError, not from any of these, so it
            # deliberately escapes: a target that stopped answering must end the
            # walk and let the caller's deadline handling take over.
            except (KdRequestError, ProtocolError, RuntimeError, ValueError) as error:
                entry["error"] = str(error)
                continue
            entry["number"] = struct.unpack_from("<H", head, PRCB_NUMBER)[0]
            entry["current_thread"] = struct.unpack_from(
                "<Q", head, PRCB_CURRENT_THREAD
            )[0]
            entry["target_set"] = struct.unpack_from("<Q", ipi, 0)[0]
            frozen_value = struct.unpack_from(
                "<I", ipi, PRCB_IPI_FROZEN - PRCB_TARGET_SET
            )[0]
            entry.update(self.describe_ipi_frozen(frozen_value))
            if entry["number"] != index:
                entry["number_mismatch"] = True
            if entry["is_freeze_owner"] and result["freeze_owner_processor"] is None:
                result["freeze_owner_processor"] = index
                result["freeze_owner_prcb"] = prcb
        return result

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
        deadline: float | None = None,
    ) -> Path:
        with self.operation_deadline(deadline):
            return self._harvest_impl(
                directory,
                stack_size=stack_size,
                object_size=object_size,
                walk_modules=walk_modules,
            )

    def _harvest_impl(
        self,
        directory: Path | str,
        *,
        stack_size: int,
        object_size: int,
        walk_modules: bool,
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
        timed_out = False
        self.last_harvest_complete = None

        def save_manifest() -> None:
            temporary = destination / "manifest.json.tmp"
            temporary.write_text(
                json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
            )
            temporary.replace(destination / "manifest.json")

        def finish() -> Path:
            self.last_harvest_complete = not timed_out
            manifest["complete"] = self.last_harvest_complete
            if timed_out:
                manifest["incomplete_reason"] = "capture deadline expired"
            save_manifest()
            self._emit("harvest-partial" if timed_out else "harvest", str(destination))
            return destination

        def capture(name: str, address: int, size: int) -> bytes | None:
            nonlocal timed_out
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
                if isinstance(error, KdTimeout) and self._operation_deadline is not None:
                    timed_out = True
                errors.append(
                    {"name": name, "address": address, "size": size, "error": str(error)}
                )
                save_manifest()
                return None

        def capture_progressive(
            name: str, address: int, size: int, chunk_size: int = 0x800
        ) -> bytes | None:
            """Persist each readable chunk and stop cleanly at an unmapped page."""
            nonlocal timed_out
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
                        if (
                            isinstance(error, KdTimeout)
                            and self._operation_deadline is not None
                        ):
                            timed_out = True
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
            if isinstance(error, KdTimeout) and self._operation_deadline is not None:
                timed_out = True
            errors.append({"name": "context-amd64", "error": str(error)})
            save_manifest()
        if timed_out:
            return finish()

        if self.last_state_payload is not None:
            (destination / "state-change.bin").write_bytes(self.last_state_payload)
            save_manifest()

        rip = registers.get("rip", self.current_state.program_counter)
        rsp = registers.get("rsp")
        capture("code", max(0, rip - 0x100), 0x300)
        if timed_out:
            return finish()
        stack_data = None
        processor_stacks: dict[int, tuple[int, bytes]] = {}
        if rsp is not None:
            stack_data = capture_progressive("stack-from-rsp", rsp, stack_size)
            if stack_data is not None:
                processor_stacks[current_cpu] = (rsp, stack_data)
            if timed_out:
                return finish()

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
                if isinstance(error, KdTimeout) and self._operation_deadline is not None:
                    timed_out = True
                errors.append(
                    {"name": name, "processor": processor, "error": str(error)}
                )
                save_manifest()
            if timed_out:
                return finish()
        for name, address, size in (
            ("current-thread", self.current_state.thread, object_size),
            ("kernel-header", self.version.kernel_base, 0x1000),
            ("loaded-module-list-head", self.version.loaded_module_list, 16),
            ("debugger-data-list-head", self.version.debugger_data_list, 16),
        ):
            capture(name, address, size)
            if timed_out:
                return finish()

        # --- Faulting process / user-mode module resolution (additive) -------
        # Every read below is guarded so a failed walk degrades gracefully and
        # never aborts the rest of the harvest.
        process_info: dict[str, object] = {}
        user_modules: list[dict[str, object]] = []
        peb = 0
        eprocess = 0
        try:
            process_info = self.get_current_process()
            eprocess = int(process_info.get("eprocess", 0) or 0)
            peb = int(process_info.get("peb", 0) or 0)
            manifest["process"] = process_info
            save_manifest()
        except (KdRequestError, KdTimeout, ProtocolError, RuntimeError) as error:
            errors.append({"name": "current-process", "error": str(error)})
            save_manifest()

        if eprocess:
            capture("current-process", eprocess & ~(PAGE_SIZE - 1), PAGE_SIZE)
            if timed_out:
                return finish()

        if peb:
            try:
                user_modules = self.get_user_modules(peb)
                manifest["user_modules"] = user_modules
                save_manifest()
            except (KdRequestError, KdTimeout, ProtocolError, RuntimeError, ValueError) as error:
                errors.append({"name": "user-modules", "error": str(error)})
                save_manifest()

        def owning_user_module(address: int) -> dict[str, object] | None:
            for module in user_modules:
                base = module.get("dll_base")
                size = module.get("size")
                if isinstance(base, int) and isinstance(size, int) and size:
                    if base <= address < base + size:
                        return module
            return None

        def map_fault_page(page_name: str, fault_va: int) -> None:
            """Capture the page containing fault_va and, if it lands inside a
            resolved user module, record module/RVA/file-offset in the manifest.
            The RVA->file-offset mapping uses the in-memory PE headers (which
            retain PointerToRawData), so it needs no on-disk copy at capture
            time."""
            page_base = fault_va & ~(PAGE_SIZE - 1)
            data = capture(page_name, page_base, PAGE_SIZE)
            record: dict[str, object] = {
                "name": page_name,
                "fault_va": fault_va,
                "page_base": page_base,
                "captured": data is not None,
            }
            module = owning_user_module(fault_va)
            if module is not None:
                module_base = int(module["dll_base"])
                rva = fault_va - module_base
                page_rva = page_base - module_base
                record["module"] = module.get("base_name") or module.get("full_name")
                record["module_full_name"] = module.get("full_name")
                record["module_base"] = module_base
                record["rva"] = rva
                record["page_rva"] = page_rva
                try:
                    header = self.read_virtual(module_base, PAGE_SIZE)
                    sections = parse_pe_sections(header)
                    section = section_for_rva(sections["sections"], rva)
                    if section is not None:
                        record["section"] = section["name"]
                        record["section_writable"] = section["writable"]
                        record["section_executable"] = section["executable"]
                        record["file_offset"] = rva_to_file_offset(
                            sections["sections"], rva
                        )
                        record["page_file_offset"] = rva_to_file_offset(
                            sections["sections"], page_rva
                        )
                    record["pe_image_base"] = sections["image_base"]
                except (KdRequestError, KdTimeout, ProtocolError, RuntimeError, ValueError) as error:
                    record["pe_error"] = str(error)
            fault_pages.append(record)
            manifest["fault_pages"] = fault_pages
            save_manifest()

        fault_pages: list[dict[str, object]] = []
        fault_rip = registers.get("rip", self.current_state.program_counter)
        try:
            map_fault_page("fault-code-page", fault_rip)
        except (KdRequestError, KdTimeout, ProtocolError, RuntimeError) as error:
            errors.append({"name": "fault-code-page", "error": str(error)})
            save_manifest()
        if timed_out:
            return finish()
        fault_data_va = self.current_state.exception_address
        if fault_data_va is not None and (
            fault_data_va & ~(PAGE_SIZE - 1)
        ) != (fault_rip & ~(PAGE_SIZE - 1)):
            try:
                map_fault_page("fault-data-page", fault_data_va)
            except (KdRequestError, KdTimeout, ProtocolError, RuntimeError) as error:
                errors.append({"name": "fault-data-page", "error": str(error)})
                save_manifest()
            if timed_out:
                return finish()

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
                if isinstance(error, KdTimeout) and self._operation_deadline is not None:
                    timed_out = True
                errors.append({"name": "loaded-modules", "error": str(error)})
            save_manifest()
            if timed_out:
                return finish()

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
            if isinstance(error, KdTimeout) and self._operation_deadline is not None:
                timed_out = True
            errors.append({"name": "debugger-data", "error": str(error)})
        if timed_out:
            return finish()

        # Per-CPU freeze state.  Without this a harvest cannot say whether the
        # APs actually froze or merely had a freeze requested, which is the
        # difference between "the debugger owns the box" and "one core is
        # wedged with interrupts off".  Additive and non-fatal by construction.
        try:
            prcbs = self.get_processor_control_blocks(
                debugger_blocks or None,
                processor_count=self.current_state.cpu_count,
            )
            manifest["processor_control_blocks"] = prcbs
            self._emit("prcb-freeze-state", _summarise_freeze_state(prcbs))
        # Deliberately wide, but never bare: KdSessionChanged means the target
        # rebooted and has to keep escaping the harvest.
        except (
            KdRequestError,
            KdTimeout,
            ProtocolError,
            NotImplementedError,
            RuntimeError,
            ValueError,
            TypeError,
            KeyError,
            IndexError,
            AttributeError,
            struct.error,
        ) as error:
            if isinstance(error, KdTimeout) and self._operation_deadline is not None:
                timed_out = True
            errors.append({"name": "processor-control-blocks", "error": str(error)})
        save_manifest()
        if timed_out:
            return finish()

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
            if isinstance(error, KdTimeout) and self._operation_deadline is not None:
                timed_out = True
            errors.append({"name": "debug-print-ring", "error": str(error)})

        return finish()

    def status(self) -> dict[str, object]:
        return {
            "host": f"{self.host}:{self.port}",
            "target": f"{self.target}:{self.port}",
            "stopped": self.stopped,
            "state": asdict(self.current_state) if self.current_state else None,
            "version": asdict(self.version) if self.version else None,
        }


def parse_pe_sections(image: bytes) -> dict[str, object]:
    """Parse the PE headers (from a file image or an in-memory header page).

    Returns image_base plus a list of sections with virtual and raw geometry
    and their characteristics.  Works on both on-disk files and image-mapped
    memory because section headers keep PointerToRawData in both.
    """
    if len(image) < 0x40 or image[0:2] != b"MZ":
        raise ValueError("not an MZ image")
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    if pe + 0x18 > len(image) or image[pe:pe + 4] != b"PE\0\0":
        raise ValueError("missing PE signature")
    number_of_sections = struct.unpack_from("<H", image, pe + 6)[0]
    size_of_optional = struct.unpack_from("<H", image, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", image, opt)[0]
    if magic == 0x20B:
        image_base = struct.unpack_from("<Q", image, opt + 24)[0]
    else:
        image_base = struct.unpack_from("<I", image, opt + 28)[0]
    section_table = opt + size_of_optional
    sections: list[dict[str, object]] = []
    for index in range(number_of_sections):
        offset = section_table + index * 40
        if offset + 40 > len(image):
            break
        raw_name = image[offset:offset + 8]
        name = raw_name.rstrip(b"\0").decode("latin-1", errors="replace")
        virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from(
            "<IIII", image, offset + 8
        )
        characteristics = struct.unpack_from("<I", image, offset + 36)[0]
        sections.append(
            {
                "name": name,
                "virtual_address": virtual_address,
                "virtual_size": virtual_size,
                "raw_pointer": raw_pointer,
                "raw_size": raw_size,
                "characteristics": characteristics,
                "writable": bool(characteristics & 0x80000000),
                "executable": bool(characteristics & 0x20000000),
            }
        )
    return {
        "image_base": image_base,
        "pe_offset": pe,
        "sections": sections,
    }


def section_for_rva(sections: list[dict[str, object]], rva: int) -> dict[str, object] | None:
    for section in sections:
        start = int(section["virtual_address"])
        size = max(int(section["virtual_size"]), int(section["raw_size"]))
        if start <= rva < start + size:
            return section
    return None


def rva_to_file_offset(sections: list[dict[str, object]], rva: int) -> int | None:
    for section in sections:
        start = int(section["virtual_address"])
        size = max(int(section["virtual_size"]), int(section["raw_size"]))
        if start <= rva < start + size:
            delta = rva - start
            if delta < int(section["raw_size"]):
                return int(section["raw_pointer"]) + delta
            return None  # falls in uninitialized (.bss-style) tail
    return None


def _pe_base_relocations(image: bytes, parsed: dict[str, object]) -> list[tuple[int, int]]:
    """Return (rva, type) for every base relocation in the on-disk image."""
    pe = int(parsed["pe_offset"])
    opt = pe + 24
    magic = struct.unpack_from("<H", image, opt)[0]
    # IMAGE_DIRECTORY_ENTRY_BASERELOC = 5
    data_dir = opt + (0x70 if magic == 0x20B else 0x60)
    reloc_rva, reloc_size = struct.unpack_from("<II", image, data_dir + 5 * 8)
    if not reloc_rva or not reloc_size:
        return []
    sections = parsed["sections"]
    assert isinstance(sections, list)
    file_off = rva_to_file_offset(sections, reloc_rva)
    if file_off is None:
        return []
    relocations: list[tuple[int, int]] = []
    end = file_off + reloc_size
    cursor = file_off
    while cursor + 8 <= end and cursor + 8 <= len(image):
        page_rva, block_size = struct.unpack_from("<II", image, cursor)
        if block_size < 8:
            break
        entries = (block_size - 8) // 2
        for i in range(entries):
            raw = struct.unpack_from("<H", image, cursor + 8 + i * 2)[0]
            reloc_type = raw >> 12
            offset = raw & 0x0FFF
            if reloc_type != 0:  # skip IMAGE_REL_BASED_ABSOLUTE padding
                relocations.append((page_rva + offset, reloc_type))
        cursor += block_size
    return relocations


def reconstruct_image_window(
    image: bytes,
    parsed: dict[str, object],
    rva_start: int,
    length: int,
    actual_base: int,
) -> tuple[bytearray, list[bool]]:
    """Build the bytes the loader WOULD have mapped for [rva_start, rva_start+length).

    Starts from the on-disk section raw bytes, then applies base relocations for
    the actual in-memory load base (delta = actual_base - preferred_base).  The
    returned ``mapped`` mask marks which bytes are backed by file data (False for
    uninitialized / .bss tail bytes that legitimately read as zero in memory).
    """
    sections = parsed["sections"]
    assert isinstance(sections, list)
    expected = bytearray(length)
    mapped = [False] * length
    for i in range(length):
        rva = rva_start + i
        section = section_for_rva(sections, rva)
        if section is None:
            continue
        delta = rva - int(section["virtual_address"])
        if delta < int(section["raw_size"]):
            file_index = int(section["raw_pointer"]) + delta
            if file_index < len(image):
                expected[i] = image[file_index]
                mapped[i] = True
    preferred_base = int(parsed["image_base"])
    reloc_delta = (actual_base - preferred_base) & ((1 << 64) - 1)
    if reloc_delta:
        window_end = rva_start + length
        for reloc_rva, reloc_type in _pe_base_relocations(image, parsed):
            if reloc_type != 10:  # only IMAGE_REL_BASED_DIR64 on amd64
                continue
            if reloc_rva + 8 <= rva_start or reloc_rva >= window_end:
                continue
            file_off = rva_to_file_offset(sections, reloc_rva)
            if file_off is None or file_off + 8 > len(image):
                continue
            original = struct.unpack_from("<Q", image, file_off)[0]
            fixed = (original + reloc_delta) & ((1 << 64) - 1)
            patch = struct.pack("<Q", fixed)
            for byte_index in range(8):
                target = reloc_rva + byte_index - rva_start
                if 0 <= target < length:
                    expected[target] = patch[byte_index]
                    mapped[target] = True
    return expected, mapped


def diff_image_page(
    captured: bytes,
    disk_image: bytes,
    module_base: int,
    page_rva: int,
) -> dict[str, object]:
    """Compare an in-memory page against the on-disk PE, relocation-aware.

    ``captured`` is the raw in-memory page (page-aligned), ``module_base`` the
    actual in-memory load base, ``page_rva`` the page's RVA (page_base -
    module_base).  Mismatches inside NON-writable sections after relocation
    adjustment are real corruption; mismatches in writable sections (.data/.tls/
    .bss) or unmapped tail bytes are reported separately as benign.
    """
    parsed = parse_pe_sections(disk_image)
    sections = parsed["sections"]
    assert isinstance(sections, list)
    length = len(captured)
    expected, mapped = reconstruct_image_window(
        disk_image, parsed, page_rva, length, module_base
    )
    real: list[dict[str, object]] = []
    benign: list[dict[str, object]] = []
    for i in range(length):
        if not mapped[i]:
            if captured[i] != 0:
                benign.append(
                    {
                        "rva": page_rva + i,
                        "reason": "unmapped-tail",
                        "memory": captured[i],
                    }
                )
            continue
        if captured[i] == expected[i]:
            continue
        rva = page_rva + i
        section = section_for_rva(sections, rva)
        record = {
            "rva": rva,
            "va": module_base + rva,
            "file_offset": rva_to_file_offset(sections, rva),
            "section": section["name"] if section else None,
            "disk": expected[i],
            "memory": captured[i],
        }
        if section is not None and not section["writable"]:
            real.append(record)
        else:
            benign.append(record)
    return {
        "module_base": module_base,
        "preferred_image_base": parsed["image_base"],
        "page_rva": page_rva,
        "page_va": module_base + page_rva,
        "length": length,
        "real_corruption_count": len(real),
        "benign_difference_count": len(benign),
        "real_corruption": real,
        "benign_differences": benign,
        "verdict": (
            "CORRUPT: read-only bytes differ from disk (USB-read corruption proven)"
            if real
            else "clean: read-only sections match disk after relocation"
        ),
    }


def diff_harvest(
    harvest_dir: Path | str,
    disk_image_path: Path | str,
    region_name: str = "fault-code-page",
) -> dict[str, object]:
    """Offline driver: diff a captured fault page in a harvest against a PE file.

    Reads the harvest manifest to find the region's captured .bin plus its
    recorded module_base/page_rva, then runs :func:`diff_image_page`.
    """
    harvest = Path(harvest_dir)
    manifest = json.loads((harvest / "manifest.json").read_text(encoding="utf-8"))
    fault_pages = manifest.get("fault_pages", [])
    entry = next((p for p in fault_pages if p.get("name") == region_name), None)
    if entry is None:
        raise ValueError(
            f"harvest has no fault page {region_name!r}; "
            f"available: {[p.get('name') for p in fault_pages]}"
        )
    if "module_base" not in entry or "page_rva" not in entry:
        raise ValueError(
            f"fault page {region_name!r} was not resolved to a user module "
            "(no module_base/page_rva recorded)"
        )
    captured = (harvest / f"{region_name}.bin").read_bytes()
    disk_image = Path(disk_image_path).read_bytes()
    result = diff_image_page(
        captured,
        disk_image,
        int(entry["module_base"]),
        int(entry["page_rva"]),
    )
    result["region"] = region_name
    result["harvest"] = str(harvest)
    result["disk_image"] = str(disk_image_path)
    result["module"] = entry.get("module")
    return result


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
    elif kind == "prcb-freeze-state":
        print(f"[KD] freeze state: {value}", file=sys.stderr, flush=True)
