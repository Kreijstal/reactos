#!/usr/bin/env python3
"""Focused compatibility tests for the native Python KDNET client."""

from __future__ import annotations

import hashlib
import io
import json
import socket
import struct
import sys
import tempfile
import time
import unittest
from contextlib import contextmanager
from dataclasses import replace
from unittest import mock
from pathlib import Path


SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))

from roskd import client as client_module  # noqa: E402
from roskd.client import (  # noqa: E402
    DBGKD_EXCEPTION_STATE_CHANGE,
    DBGKD_GET_STRING_API,
    DBGKD_LOAD_SYMBOLS_STATE_CHANGE,
    KdDebuggerDataBlock,
    KdModule,
    KdNetClient,
    KdRequestError,
    KdSessionChanged,
    KdStateChange,
    KdTimeout,
    KdVersion,
    parse_amd64_context,
    parse_amd64_loader_entry,
    parse_state_change,
)
from roskd import cli as roskd_cli  # noqa: E402
from roskd.protocol import (  # noqa: E402
    KD_INITIAL_ID,
    KD_LEADER_CONTROL,
    KD_LEADER_DATA,
    KD_SYNC_ID,
    KD_TYPE_ACKNOWLEDGE,
    KD_TYPE_DEBUG_IO,
    KD_TYPE_STATE_CHANGE64,
    KD_TYPE_STATE_MANIPULATE,
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


CONTROL_PACKET_VECTOR = bytes.fromhex(
    "4d44424701014e8d336f89b7f38dc1f2d1fa1f18dd3cdf95"
    "9871b89d3428b60d8735842ae6584db38f800dd2211b0b08"
    "1a41bfb854d6"
)
DATA_PACKET_VECTOR = bytes.fromhex(
    "4d444247010057666634685d355db83b79f2252ca614a0f4"
    "2402e4de529399d11e25363a93c6"
)
DATA_KEY_VECTOR = bytes.fromhex(
    "9ef2a5ae49151ad8cea38aad9601ba8da241d6cd63493d5c"
    "2a5a72f55100745c"
)
HANDSHAKE_DATA_KEY_VECTOR = bytes.fromhex(
    "faa594d4813f0c9b1fa48cd949713d8722cf5d4dafec65c7"
    "3bc6d23ffb22a696"
)
KD_DATA_PACKET_VECTOR = bytes.fromhex(
    "3030303002000500000880800f0000000102030405"
)
KD_ACK_PACKET_VECTOR = bytes.fromhex(
    "69696969040000000000808000000000"
)


def _free_udp_port() -> int:
    """A port free on both loopback addresses the KDNET client pairs up."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.2", port))
    return port


class KdNetCryptoTests(unittest.TestCase):
    def test_key_derivation_and_validation(self) -> None:
        crypto = KdNetCrypto("1.2.3.4")
        self.assertEqual(crypto.control_key[::8], b"\x01\x02\x03\x04")
        self.assertEqual(crypto.hmac_key[::8], b"\xfe\xfd\xfc\xfb")
        for invalid in ("1.2.3", "1.2.3.4.5", "1.2.*.4", "1_2.2.3.4"):
            with self.subTest(invalid=invalid), self.assertRaises(ProtocolError):
                KdNetCrypto(invalid)

    def test_control_reference_vector(self) -> None:
        crypto = KdNetCrypto("1.2.3.4")
        payload = bytes(range(21))
        packet = crypto.encode(
            payload,
            version=1,
            packet_type=KDNET_TYPE_CONTROL,
            sequence=0x01020304050607,
            direction=KDNET_DIRECTION_TARGET,
        )
        self.assertEqual(packet, CONTROL_PACKET_VECTOR)
        decoded = crypto.decode(packet)
        self.assertEqual(decoded.payload, payload)
        self.assertEqual(decoded.sequence, 0x01020304050607)
        self.assertEqual(decoded.padding, 3)

    def test_data_reference_vector(self) -> None:
        crypto = KdNetCrypto("1.2.3.4")
        response = bytes((index * 7 + 3) & 0xFF for index in range(322))
        self.assertEqual(crypto.derive_data_key(response), DATA_KEY_VECTOR)
        packet = crypto.encode(
            b"b",
            version=1,
            packet_type=KDNET_TYPE_DATA,
            sequence=9,
            direction=KDNET_DIRECTION_TARGET,
        )
        self.assertEqual(packet, DATA_PACKET_VECTOR)
        self.assertEqual(crypto.decode(packet).payload, b"b")

    def test_handshake_key_vector(self) -> None:
        crypto = KdNetCrypto("1.2.3.4")
        client = bytes(range(32))
        host = bytes(range(0xA0, 0xC0))
        response = crypto.build_response(client, host)
        self.assertEqual(len(response), 322)
        self.assertEqual(crypto.data_key, HANDSHAKE_DATA_KEY_VECTOR)

    def test_corruption_is_rejected(self) -> None:
        crypto = KdNetCrypto("1.2.3.4")
        packet = bytearray(CONTROL_PACKET_VECTOR)
        packet[10] ^= 1
        with self.assertRaisesRegex(ProtocolError, "authentication"):
            crypto.decode(bytes(packet))


class InnerKdTests(unittest.TestCase):
    def test_data_reference_vector(self) -> None:
        packet = encode_kd_data(
            KD_TYPE_STATE_MANIPULATE,
            KD_INITIAL_ID | KD_SYNC_ID,
            b"\x01\x02\x03\x04\x05",
        )
        self.assertEqual(packet, KD_DATA_PACKET_VECTOR)
        decoded = decode_kd_packet(packet)
        self.assertEqual(decoded.leader, KD_LEADER_DATA)
        self.assertEqual(decoded.payload, b"\x01\x02\x03\x04\x05")

    def test_ack_reference_vector(self) -> None:
        packet = encode_kd_control(KD_TYPE_ACKNOWLEDGE, KD_INITIAL_ID)
        self.assertEqual(packet, KD_ACK_PACKET_VECTOR)
        self.assertEqual(decode_kd_packet(packet).leader, KD_LEADER_CONTROL)

    def test_bad_checksum_is_rejected(self) -> None:
        packet = bytearray(KD_DATA_PACKET_VECTOR)
        packet[-1] ^= 1
        with self.assertRaisesRegex(ProtocolError, "checksum"):
            decode_kd_packet(bytes(packet))


class StructureTests(unittest.TestCase):
    @staticmethod
    def _breakpoint_stop_client(
        program_counter: int,
        exception_address: int,
        opcode: bytes = b"\xcc",
    ) -> tuple[KdNetClient, list, list]:
        client = object.__new__(KdNetClient)
        client._manual_break_stop = False
        client._planted_breakpoints = {}
        client.stopped = True
        client.current_state = KdStateChange(
            state=DBGKD_EXCEPTION_STATE_CHANGE,
            cpu_level=6,
            cpu=0,
            cpu_count=4,
            thread=0x1234,
            program_counter=program_counter,
            exception_code=0x80000003,
            exception_address=exception_address,
        )
        client.version = KdVersion(12, 3790, 6, 2, 7, 0x8664, 0, 0, 0)
        context = bytearray(0x4D0)
        struct.pack_into("<Q", context, 0xF8, program_counter)
        written: list = []
        events: list = []
        client.read_virtual = lambda address, size: opcode[:size]
        client.get_context = lambda: bytes(context)
        client.set_context = written.append
        client._emit = lambda kind, value: events.append((kind, value))
        return client, written, events

    def test_break_resume_steps_over_int3_when_pc_equals_exception_address(
        self,
    ) -> None:
        """The Windows-accurate kernel reports ExceptionAddress == PC.

        ntoskrnl/ke/amd64/trap.S points ExceptionAddress at the int3 opcode for
        BREAKPOINT_BREAK, matching i386 and the ntdll:exception int3_handler, so
        the resume fix-up may not key off ExceptionAddress == PC + 1.
        """
        pc = 0xFFFFF80000592AA2
        client, written, events = self._breakpoint_stop_client(pc, pc)

        client._prepare_manual_break_resume()

        self.assertEqual(len(written), 1)
        self.assertEqual(struct.unpack_from("<Q", written[0], 0xF8)[0], pc + 1)
        self.assertEqual(events[0][0], "resume-fixup")

    def test_manual_break_resume_advances_rewound_amd64_rip(self) -> None:
        pc = 0xFFFFF80000102030
        client, written, events = self._breakpoint_stop_client(pc, pc + 1)
        client._manual_break_stop = True

        client._prepare_manual_break_resume()

        self.assertFalse(client._manual_break_stop)
        self.assertEqual(len(written), 1)
        self.assertEqual(struct.unpack_from("<Q", written[0], 0xF8)[0], pc + 1)
        self.assertEqual(events[0][0], "resume-fixup")

    def test_break_resume_uses_legacy_signature_when_opcode_unreadable(
        self,
    ) -> None:
        pc = 0xFFFFF80000102030
        client, written, _ = self._breakpoint_stop_client(pc, pc + 1, opcode=b"")

        client._prepare_manual_break_resume()

        self.assertEqual(len(written), 1)
        self.assertEqual(struct.unpack_from("<Q", written[0], 0xF8)[0], pc + 1)

    def test_break_resume_leaves_a_planted_breakpoint_alone(self) -> None:
        pc = 0xFFFFF80000102030
        client, written, _ = self._breakpoint_stop_client(pc, pc)
        client._planted_breakpoints = {1: pc}

        client._prepare_manual_break_resume()

        self.assertEqual(written, [])

    def test_break_resume_leaves_a_software_raised_status_alone(self) -> None:
        """A STATUS_BREAKPOINT whose PC is not on a 0xCC never executed one."""
        pc = 0xFFFFF80000102030
        client, written, _ = self._breakpoint_stop_client(pc, pc + 1, opcode=b"\x90")

        client._prepare_manual_break_resume()

        self.assertEqual(written, [])

    def test_break_resume_does_nothing_while_the_target_runs(self) -> None:
        pc = 0xFFFFF80000102030
        client, written, _ = self._breakpoint_stop_client(pc, pc)
        client.stopped = False

        client._prepare_manual_break_resume()

        self.assertEqual(written, [])

    def test_continue_ack_loss_does_not_leave_client_marked_stopped(self) -> None:
        client = object.__new__(KdNetClient)
        client._manual_break_stop = False
        client._planted_breakpoints = {}
        client.current_state = None
        client.stopped = True
        sent = []
        client._send_inner_data = lambda packet_type, payload: sent.append(
            (packet_type, payload)
        )

        def lose_ack(_expected: tuple[int, ...]) -> None:
            raise KdTimeout("continue ACK lost")

        client._wait_for = lose_ack
        with self.assertRaisesRegex(KdTimeout, "ACK lost"):
            client.continue_execution()

        self.assertEqual(len(sent), 1)
        self.assertFalse(client.stopped)

    def test_io_space_requests_validate_and_decode_width(self) -> None:
        client = object.__new__(KdNetClient)
        requests = []

        def request(api: int, union: bytes) -> bytes:
            address, size, value = struct.unpack("<QII", union)
            requests.append((api, address, size, value))
            result = 0xA5A51234 if api == 0x3139 else value
            return bytes(16) + struct.pack("<QII", address, size, result)

        client._request = request

        self.assertEqual(client.read_io(0xCFC, 2), 0x1234)
        self.assertEqual(client.write_io(0xCF8, 4, 0x80009AA0), 4)
        self.assertEqual(
            requests,
            [
                (0x3139, 0xCFC, 2, 0),
                (0x313A, 0xCF8, 4, 0x80009AA0),
            ],
        )
        for size in (0, 3, 8):
            with self.subTest(size=size), self.assertRaises(ValueError):
                client.read_io(0xCFC, size)
        with self.assertRaises(ValueError):
            client.write_io(0xCFC, 1, 0x100)

    def test_module_load_resume_does_not_change_context(self) -> None:
        client = object.__new__(KdNetClient)
        client._manual_break_stop = True
        client._planted_breakpoints = {}
        client.stopped = True
        client.current_state = KdStateChange(
            state=DBGKD_LOAD_SYMBOLS_STATE_CHANGE,
            cpu_level=6,
            cpu=0,
            cpu_count=4,
            thread=0x1234,
            program_counter=0xFFFFF80000102030,
        )
        client.version = KdVersion(12, 3790, 6, 2, 7, 0x8664, 0, 0, 0)
        client.read_virtual = lambda address, size: self.fail("module load probe")
        client.get_context = lambda: self.fail("module load context read")

        client._prepare_manual_break_resume()

        self.assertFalse(client._manual_break_stop)

    def test_debug_prompt_gets_break_once_response(self) -> None:
        client = object.__new__(KdNetClient)
        client.event_callback = None
        client._log = None
        sent = []
        client._send_inner_data = lambda packet_type, payload: sent.append(
            (packet_type, payload)
        )
        request = (
            struct.pack("<IHHII", DBGKD_GET_STRING_API, 6, 2, 7, 2)
            + b"prompt?"
        )

        client._handle_debug_io(
            KdPacket(KD_LEADER_DATA, KD_TYPE_DEBUG_IO, KD_INITIAL_ID, request)
        )

        self.assertEqual(len(sent), 1)
        packet_type, response = sent[0]
        self.assertEqual(packet_type, KD_TYPE_DEBUG_IO)
        self.assertEqual(
            struct.unpack_from("<IHHII", response),
            (DBGKD_GET_STRING_API, 6, 2, 0, 1),
        )
        self.assertEqual(response[16:], b"o")

    def test_changed_client_key_aborts_active_session(self) -> None:
        client = object.__new__(KdNetClient)
        client.client_key = b"a" * 32
        client.host_key = b"h" * 32
        client.outer_send_sequence = 9
        client.inner_send_id = KD_INITIAL_ID
        client.last_inner_sent = b"request"
        client.last_received_signature = (1, 2, 3)
        client.current_state = object()
        client.last_state_payload = b"state"
        client.observed_modules = {1: object()}
        client.version = object()
        client.stopped = True
        client._manual_break_stop = True
        client._planted_breakpoints = {1: 0xFFFFF80000102030}
        client._request_in_flight = True
        client.last_harvest_complete = True
        client.crypto = type(
            "CryptoStub",
            (),
            {"build_response": staticmethod(lambda _client, _host: b"response")},
        )()
        sent = []
        events = []
        client._send_outer = lambda payload, packet_type, sequence: sent.append(
            (payload, packet_type, sequence)
        )
        client._emit = lambda kind, value: events.append((kind, value))

        with self.assertRaises(KdSessionChanged):
            client._handle_poke(b"\x01\x01" + b"b" * 32, 1, 7)

        self.assertEqual(sent, [(b"response", KDNET_TYPE_CONTROL, 7)])
        self.assertEqual(events[-2][0], "session-change")
        self.assertEqual(
            events[-1],
            (
                "handshake",
                "new target session client_key_sha256="
                + hashlib.sha256(b"b" * 32).hexdigest(),
            ),
        )
        self.assertFalse(client._request_in_flight)

    def test_session_state_round_trip_preserves_data_key(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "session.json"
            client = KdNetClient(
                "127.0.0.1",
                "127.0.0.2",
                50000,
                "1.2.3.4",
                session_state=path,
            )
            sent = []
            client._send_outer = (
                lambda payload, packet_type, *, sequence=None: sent.append(
                    (payload, packet_type, sequence)
                )
            )
            client._handle_poke(b"\x01\x01" + bytes(range(32)), 7, 9)

            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            self.assertEqual(len(sent), 1)
            saved_data_key = client.crypto.data_key
            saved_host_key = client.host_key

            restored = KdNetClient(
                "127.0.0.1",
                "127.0.0.2",
                50000,
                "1.2.3.4",
                session_state=path,
            )
            self.assertEqual(restored.client_key, bytes(range(32)))
            self.assertEqual(restored.host_key, saved_host_key)
            self.assertEqual(restored.crypto.data_key, saved_data_key)
            self.assertEqual(restored.version_number, 7)

    def test_connect_resumes_persisted_session_before_waiting_for_poke(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "session.json"
            original = KdNetClient(
                "127.0.0.1",
                "127.0.0.2",
                50000,
                "1.2.3.4",
                session_state=path,
            )
            original._send_outer = lambda *_args, **_kwargs: None
            original._handle_poke(b"\x01\x01" + bytes(range(32)), 1, 4)

            client = KdNetClient(
                "127.0.0.1",
                "127.0.0.2",
                50000,
                "1.2.3.4",
                session_state=path,
            )
            client._open_socket = lambda: None
            sent = []
            client._send_outer = lambda payload, packet_type: sent.append(
                (payload, packet_type)
            )
            packet = type(
                "StatePacket",
                (),
                {"packet_type": KD_TYPE_STATE_CHANGE64},
            )()
            client._wait_for = lambda expected, timeout: packet
            state = object()
            version = object()
            client._record_state = lambda _packet: state
            client.get_version = lambda: version

            self.assertIs(client.connect(1.0), state)
            self.assertEqual(sent, [(b"b", KDNET_TYPE_DATA)])
            self.assertIs(client.version, version)
            self.assertTrue(client.stopped)
            self.assertTrue(client._manual_break_stop)

    def test_pending_manipulate_response_is_drained(self) -> None:
        client = object.__new__(KdNetClient)
        client._request_in_flight = True
        waits = []
        client._wait_for = lambda expected, timeout: waits.append((expected, timeout))

        client.drain_pending_request(timeout=2.5)

        self.assertEqual(waits, [((KD_TYPE_STATE_MANIPULATE,), 2.5)])
        self.assertFalse(client._request_in_flight)

    def test_manipulate_request_can_target_another_processor(self) -> None:
        client = object.__new__(KdNetClient)
        client.current_state = None
        payload = client._build_manipulate(0x3132, processor=3)
        api, cpu_level, processor = struct.unpack_from("<IHH", payload)
        self.assertEqual(api, 0x3132)
        self.assertEqual(cpu_level, 0)
        self.assertEqual(processor, 3)

    def test_exception_state(self) -> None:
        payload = bytearray(80)
        struct.pack_into("<IHHI", payload, 0, DBGKD_EXCEPTION_STATE_CHANGE, 6, 2, 4)
        struct.pack_into("<QQ", payload, 16, 0x1234, 0xFFFFF80000102030)
        struct.pack_into("<I", payload, 32, 0x80000003)
        struct.pack_into("<Q", payload, 48, 0xFFFFF80000102030)
        state = parse_state_change(bytes(payload))
        self.assertEqual(state.cpu, 2)
        self.assertEqual(state.cpu_count, 4)
        self.assertEqual(state.exception_code, 0x80000003)
        self.assertEqual(state.exception_address, state.program_counter)

    def test_exception_state_carries_the_amd64_instruction_stream(self) -> None:
        """A full AMD64 state change reports the bytes at ProgramCounter.

        Layout of a real 244-byte packet: the fixed-size exception union ends at
        0xC0, AMD64_DBGKD_CONTROL_REPORT puts InstructionCount at 0xD4 and
        InstructionStream at 0xD8.
        """
        payload = bytearray(244)
        struct.pack_into("<IHHI", payload, 0, DBGKD_EXCEPTION_STATE_CHANGE, 6, 0, 4)
        struct.pack_into("<QQ", payload, 16, 0x1234, 0xFFFFF80000592AA2)
        struct.pack_into("<I", payload, 32, 0x80000003)
        struct.pack_into("<Q", payload, 48, 0xFFFFF80000592AA2)
        struct.pack_into("<H", payload, 0xD4, 16)
        payload[0xD8:0xD8 + 16] = bytes.fromhex("ccc34c89c0cd2dccc389c84889d14489")
        state = parse_state_change(bytes(payload))
        self.assertEqual(
            state.amd64_instruction_stream, "ccc34c89c0cd2dccc389c84889d14489"
        )

    def test_break_resume_prefers_the_reported_instruction_stream(self) -> None:
        pc = 0xFFFFF80000592AA2
        client, written, _ = self._breakpoint_stop_client(pc, pc)
        client.current_state = replace(
            client.current_state, amd64_instruction_stream="ccc3"
        )
        client.read_virtual = lambda address, size: self.fail("needless memory read")

        client._prepare_manual_break_resume()

        self.assertEqual(len(written), 1)
        self.assertEqual(struct.unpack_from("<Q", written[0], 0xF8)[0], pc + 1)

    def test_module_state(self) -> None:
        path = b"acpi.sys"
        payload = bytearray(65 + len(path))
        struct.pack_into("<IHHI", payload, 0, DBGKD_LOAD_SYMBOLS_STATE_CHANGE, 6, 0, 4)
        struct.pack_into("<QQ", payload, 16, 0x1000, 0x2000)
        struct.pack_into("<I", payload, 32, len(path))
        struct.pack_into("<Q", payload, 40, 0xFFFFF88001000000)
        struct.pack_into("<I", payload, 60, 0x12000)
        payload[64] = 0
        payload[-len(path):] = path
        state = parse_state_change(bytes(payload))
        self.assertEqual(state.module_path, "acpi.sys")
        self.assertEqual(state.module_size, 0x12000)

    def test_amd64_context(self) -> None:
        context = bytearray(0x100)
        struct.pack_into("<Q", context, 0x78, 0x11)
        struct.pack_into("<Q", context, 0x98, 0x22)
        struct.pack_into("<Q", context, 0xF8, 0x33)
        registers = parse_amd64_context(bytes(context))
        self.assertEqual(registers["rax"], 0x11)
        self.assertEqual(registers["rsp"], 0x22)
        self.assertEqual(registers["rip"], 0x33)

    def test_amd64_loader_entry(self) -> None:
        raw = bytearray(0x70)
        struct.pack_into("<QQ", raw, 0, 0x1111, 0x2222)
        struct.pack_into("<QQI", raw, 0x30, 0xFFFFF80000400000, 0x3333, 0x4000)
        struct.pack_into("<H", raw, 0x48, 24)
        struct.pack_into("<Q", raw, 0x50, 0x4444)
        struct.pack_into("<H", raw, 0x58, 16)
        struct.pack_into("<Q", raw, 0x60, 0x5555)
        entry = parse_amd64_loader_entry(0xAAAA, bytes(raw))
        self.assertEqual(entry["entry"], 0xAAAA)
        self.assertEqual(entry["flink"], 0x1111)
        self.assertEqual(entry["dll_base"], 0xFFFFF80000400000)
        self.assertEqual(entry["size"], 0x4000)
        self.assertEqual(entry["base_name_buffer"], 0x5555)


class HarvestTests(unittest.TestCase):
    def test_stack_is_persisted_before_unmapped_page(self) -> None:
        client = object.__new__(KdNetClient)
        client.stopped = True
        client.current_state = KdStateChange(
            state=DBGKD_EXCEPTION_STATE_CHANGE,
            cpu_level=6,
            cpu=0,
            cpu_count=1,
            thread=0x3000,
            program_counter=0x1100,
        )
        client.version = KdVersion(12, 3790, 6, 2, 7, 0x8664, 0x1000, 0x4000, 0x5000)
        client.last_state_payload = b"state"
        client.observed_modules = {
            0x1000: KdModule(0, 0x1000, 0, 0x1000, "kernel", "kernel")
        }
        client.event_callback = None
        client._log = None

        context = bytearray(0x4D0)
        struct.pack_into("<Q", context, 0x98, 0x1D00)
        struct.pack_into("<Q", context, 0xF8, 0x1100)
        client.get_context = lambda processor=None: bytes(context)

        def read_virtual(address: int, size: int) -> bytes:
            if address == 0x2000:
                raise KdRequestError(0x3130, 0xC0000001)
            return bytes((address + offset) & 0xFF for offset in range(size))

        client.read_virtual = read_virtual
        client.get_debugger_data_blocks = lambda: []

        with tempfile.TemporaryDirectory() as temporary:
            destination = client.harvest(Path(temporary), stack_size=0x1000)
            stack = (destination / "stack-from-rsp.bin").read_bytes()
            manifest = json.loads((destination / "manifest.json").read_text())

        self.assertEqual(len(stack), 0x300)
        stack_region = next(
            region for region in manifest["regions"]
            if region["name"] == "stack-from-rsp"
        )
        self.assertEqual(stack_region["address"], 0x1D00)
        self.assertEqual(stack_region["size"], 0x300)
        stack_error = next(
            error for error in manifest["errors"]
            if error["name"] == "stack-from-rsp"
        )
        self.assertEqual(stack_error["captured_size"], 0x300)



# Real harvests captured from the ASUS target.  Used read-only, as replay
# fixtures for the KDBG field offsets; absent on a fresh checkout, where the
# synthetic fixtures below still cover every code path.
HARVEST_CORPUS = Path("/tmp/asus-roskd-harvest-current")


def _captured_kdbg_blocks() -> list[tuple[Path, bytes, dict]]:
    if not HARVEST_CORPUS.is_dir():
        return []
    captured = []
    for directory in sorted(HARVEST_CORPUS.iterdir()):
        block = directory / "debugger-data-00-KDBG.bin"
        manifest = directory / "manifest.json"
        if not block.is_file() or not manifest.is_file():
            continue
        try:
            captured.append(
                (
                    directory,
                    block.read_bytes(),
                    json.loads(manifest.read_text(encoding="utf-8")),
                )
            )
        except (OSError, ValueError):
            continue
    return captured


CAPTURED_KDBG = _captured_kdbg_blocks()


class PrcbCaptureTests(unittest.TestCase):
    """Per-CPU freeze state read through KDBG's KiProcessorBlock."""

    ARRAY = 0xFFFFF80000680000
    PRCBS = (
        0xFFFFF78000001000,
        0xFFFFF78000005000,
        0xFFFFF78000009000,
        0xFFFFF7800000D000,
    )

    @classmethod
    def _kdbg(
        cls,
        *,
        array: int | None = None,
        size_prcb: int = client_module.PRCB_SIZE,
        offset_number: int = client_module.PRCB_NUMBER,
        offset_thread: int = client_module.PRCB_CURRENT_THREAD,
        length: int = 0x360,
    ) -> KdDebuggerDataBlock:
        data = bytearray(length)
        if length >= client_module.KDBG_KI_PROCESSOR_BLOCK + 8:
            struct.pack_into(
                "<Q",
                data,
                client_module.KDBG_KI_PROCESSOR_BLOCK,
                cls.ARRAY if array is None else array,
            )
        if length >= client_module.KDBG_PRCB_METRICS_MINIMUM:
            struct.pack_into("<H", data, client_module.KDBG_SIZE_PRCB, size_prcb)
            struct.pack_into(
                "<H",
                data,
                client_module.KDBG_OFFSET_PRCB_CURRENT_THREAD,
                offset_thread,
            )
            struct.pack_into(
                "<H", data, client_module.KDBG_OFFSET_PRCB_NUMBER, offset_number
            )
        return KdDebuggerDataBlock(
            address=0xFFFFF80000690000,
            owner_tag="KDBG",
            size=len(data),
            data=bytes(data),
        )

    @classmethod
    def _client(
        cls,
        *,
        pointers: tuple[int, ...] | None = None,
        frozen: tuple[int, ...] = (
            client_module.IPI_FROZEN_STATE_OWNER
            | client_module.IPI_FROZEN_FLAG_ACTIVE,
            client_module.IPI_FROZEN_STATE_FROZEN,
            client_module.IPI_FROZEN_STATE_FROZEN,
            client_module.IPI_FROZEN_STATE_TARGET_FREEZE,
        ),
        numbers: tuple[int, ...] | None = None,
        unreadable: frozenset[int] = frozenset(),
        short_array: bool = False,
    ) -> KdNetClient:
        pointers = cls.PRCBS if pointers is None else pointers
        numbers = tuple(range(len(pointers))) if numbers is None else numbers
        array_blob = struct.pack(f"<{len(pointers)}Q", *pointers)
        if short_array:
            array_blob = array_blob[:8]

        client = object.__new__(KdNetClient)
        client.version = KdVersion(12, 3790, 6, 2, 7, 0x8664, 0x1000, 0x4000, 0x5000)
        client.current_state = KdStateChange(
            state=DBGKD_EXCEPTION_STATE_CHANGE,
            cpu_level=6,
            cpu=0,
            cpu_count=len(pointers),
            thread=0x3000,
            program_counter=0x1100,
        )

        def read_virtual(address: int, size: int) -> bytes:
            if address in unreadable:
                raise KdRequestError(0x3130, 0xC0000001)
            if address == cls.ARRAY:
                return array_blob[:size]
            for index, prcb in enumerate(pointers):
                if address == prcb:
                    head = bytearray(client_module.PRCB_HEAD_SIZE)
                    struct.pack_into(
                        "<H", head, client_module.PRCB_NUMBER, numbers[index]
                    )
                    struct.pack_into(
                        "<Q",
                        head,
                        client_module.PRCB_CURRENT_THREAD,
                        0xFFFFFA8000000000 + index,
                    )
                    return bytes(head[:size])
                if address == prcb + client_module.PRCB_TARGET_SET:
                    blob = bytearray(client_module.PRCB_IPI_SLICE_SIZE)
                    struct.pack_into("<Q", blob, 0, 0xF)
                    struct.pack_into(
                        "<I",
                        blob,
                        client_module.PRCB_IPI_FROZEN
                        - client_module.PRCB_TARGET_SET,
                        frozen[index],
                    )
                    return bytes(blob[:size])
            return bytes(size)

        client.read_virtual = read_virtual
        return client

    def test_freeze_state_is_decoded_per_processor(self) -> None:
        result = self._client().get_processor_control_blocks([self._kdbg()])

        self.assertEqual(result["processor_block_array"], self.ARRAY)
        self.assertEqual(result["layout_warnings"], [])
        self.assertEqual(result["freeze_owner_processor"], 0)
        self.assertEqual(result["freeze_owner_prcb"], self.PRCBS[0])
        states = [entry["state"] for entry in result["processors"]]
        self.assertEqual(states, ["freeze-owner", "frozen", "frozen", "target-freeze"])
        # The owner is the one that entered the debugger; cpu3 only ever had a
        # freeze *requested*, which is the distinction the capture exists for.
        self.assertTrue(result["processors"][0]["active"])
        self.assertTrue(result["processors"][1]["is_frozen"])
        self.assertFalse(result["processors"][3]["is_frozen"])
        self.assertTrue(result["processors"][3]["freeze_requested"])
        self.assertEqual(
            result["processors"][2]["current_thread"], 0xFFFFFA8000000002
        )
        self.assertEqual([entry["number"] for entry in result["processors"]],
                         [0, 1, 2, 3])
        self.assertNotIn("error", result)

    def test_a_bogus_prcb_pointer_is_reported_not_dereferenced(self) -> None:
        reads: list[int] = []
        client = self._client(
            pointers=(self.PRCBS[0], 0, 0x1234, self.PRCBS[3] + 1)
        )
        inner = client.read_virtual

        def tracking(address: int, size: int) -> bytes:
            reads.append(address)
            return inner(address, size)

        client.read_virtual = tracking

        result = client.get_processor_control_blocks([self._kdbg()])

        self.assertEqual(result["processors"][0]["state"], "freeze-owner")
        self.assertIn("NULL", result["processors"][1]["error"])
        self.assertIn("implausible", result["processors"][2]["error"])
        self.assertIn("implausible", result["processors"][3]["error"])
        # A user-mode-looking or misaligned pointer must never be read.
        self.assertNotIn(0x1234, reads)
        self.assertNotIn(self.PRCBS[3] + 1, reads)
        # cpu0 was still readable, so the owner is still recoverable.
        self.assertEqual(result["freeze_owner_processor"], 0)

    def test_an_unreadable_prcb_does_not_stop_the_walk(self) -> None:
        client = self._client(unreadable=frozenset({self.PRCBS[1]}))

        result = client.get_processor_control_blocks([self._kdbg()])

        self.assertEqual(len(result["processors"]), 4)
        self.assertIn("0xc0000001", result["processors"][1]["error"])
        self.assertEqual(result["processors"][2]["state"], "frozen")
        self.assertEqual(result["freeze_owner_processor"], 0)

    def test_an_unreadable_freeze_word_does_not_stop_the_walk(self) -> None:
        client = self._client(
            unreadable=frozenset(
                {self.PRCBS[2] + client_module.PRCB_TARGET_SET}
            )
        )

        result = client.get_processor_control_blocks([self._kdbg()])

        self.assertIn("0xc0000001", result["processors"][2]["error"])
        self.assertNotIn("state", result["processors"][2])
        self.assertEqual(result["processors"][3]["state"], "target-freeze")

    def test_a_short_processor_block_read_is_reported(self) -> None:
        result = self._client(short_array=True).get_processor_control_blocks(
            [self._kdbg()]
        )

        self.assertIn("short KiProcessorBlock read", result["error"])
        self.assertEqual(len(result["processors"]), 1)
        self.assertEqual(result["processors"][0]["state"], "freeze-owner")

    def test_a_bogus_processor_block_array_is_never_dereferenced(self) -> None:
        client = self._client()
        client.read_virtual = lambda address, size: self.fail(
            f"read 0x{address:x} despite a bogus KiProcessorBlock"
        )

        result = client.get_processor_control_blocks(
            [self._kdbg(array=0x0000000000401000)]
        )

        self.assertIn("not a kernel pointer", result["error"])
        self.assertEqual(result["processors"], [])

    def test_a_divergent_kprcb_layout_is_flagged(self) -> None:
        """ReactOS' KPRCB is not Windows 10's, so IpiFrozen cannot be assumed.

        The target publishes SizePrcb/OffsetPrcbNumber/OffsetPrcbCurrentThread;
        if those disagree with the offsets this client was built against then
        PRCB_IPI_FROZEN is a guess and the manifest has to say so.
        """
        result = self._client().get_processor_control_blocks(
            [self._kdbg(size_prcb=0x9000, offset_number=0x18)]
        )

        self.assertEqual(len(result["layout_warnings"]), 2)
        self.assertTrue(
            any("SizePrcb" in warning for warning in result["layout_warnings"])
        )
        self.assertTrue(
            any(
                "OffsetPrcbNumber" in warning
                for warning in result["layout_warnings"]
            )
        )
        # The read still happens: a flagged reading beats no reading at all.
        self.assertEqual(result["processors"][0]["state"], "freeze-owner")

    def test_a_prcb_number_mismatch_is_flagged(self) -> None:
        result = self._client(numbers=(0, 1, 7, 3)).get_processor_control_blocks(
            [self._kdbg()]
        )

        self.assertNotIn("number_mismatch", result["processors"][1])
        self.assertTrue(result["processors"][2]["number_mismatch"])

    def test_a_truncated_kdbg_is_rejected(self) -> None:
        with self.assertRaisesRegex(ProtocolError, "too old"):
            self._client().get_processor_control_blocks([self._kdbg(length=0x220)])

    def test_a_missing_kdbg_is_rejected(self) -> None:
        with self.assertRaisesRegex(ProtocolError, "KDBG"):
            self._client().get_processor_control_blocks([])

    def test_harvest_records_the_freeze_state(self) -> None:
        client = self._client()
        client.stopped = True
        client.last_state_payload = b"state"
        client.observed_modules = {}
        client.event_callback = None
        client._log = None
        context = bytearray(0x4D0)
        struct.pack_into("<Q", context, 0x98, 0x1D00)
        struct.pack_into("<Q", context, 0xF8, 0x1100)
        client.get_context = lambda processor=None: bytes(context)
        block = self._kdbg()
        client.get_debugger_data_blocks = lambda: [block]

        with tempfile.TemporaryDirectory() as temporary:
            destination = client.harvest(Path(temporary), stack_size=0x100)
            manifest = json.loads((destination / "manifest.json").read_text())

        prcbs = manifest["processor_control_blocks"]
        self.assertEqual(prcbs["freeze_owner_processor"], 0)
        self.assertEqual(
            [entry["state"] for entry in prcbs["processors"]],
            ["freeze-owner", "frozen", "frozen", "target-freeze"],
        )
        self.assertEqual(
            prcbs["offset_prcb_ipi_frozen"], client_module.PRCB_IPI_FROZEN
        )
        self.assertNotIn(
            "processor-control-blocks",
            [error["name"] for error in manifest["errors"]],
        )

    def test_harvest_survives_a_target_without_a_kdbg(self) -> None:
        """A harvest must never abort because the freeze state is unavailable."""
        client = self._client()
        client.stopped = True
        client.last_state_payload = b"state"
        client.observed_modules = {}
        client.event_callback = None
        client._log = None
        context = bytearray(0x4D0)
        struct.pack_into("<Q", context, 0x98, 0x1D00)
        struct.pack_into("<Q", context, 0xF8, 0x1100)
        client.get_context = lambda processor=None: bytes(context)
        client.get_debugger_data_blocks = lambda: []

        with tempfile.TemporaryDirectory() as temporary:
            destination = client.harvest(Path(temporary), stack_size=0x100)
            manifest = json.loads((destination / "manifest.json").read_text())

        self.assertNotIn("processor_control_blocks", manifest)
        error = next(
            entry for entry in manifest["errors"]
            if entry["name"] == "processor-control-blocks"
        )
        self.assertIn("KDBG", error["error"])
        self.assertTrue(manifest["complete"])

    def test_summary_line_survives_a_hole_in_the_reading(self) -> None:
        summary = client_module._summarise_freeze_state(
            {
                "processors": [
                    {"processor": 0, "state": "freeze-owner", "active": True},
                    {"processor": 1, "error": "unmapped"},
                ],
                "freeze_owner_processor": 0,
                "layout_warnings": ["SizePrcb=0x1 but this build expects 0x42a0"],
            }
        )

        self.assertIn("cpu0=freeze-owner+active", summary)
        self.assertIn("cpu1=?(unmapped)", summary)
        self.assertIn("owner=cpu0", summary)
        self.assertIn("LAYOUT-MISMATCH", summary)

    @unittest.skipUnless(
        CAPTURED_KDBG,
        f"no captured harvests under {HARVEST_CORPUS} to replay",
    )
    def test_captured_kdbg_blocks_confirm_the_field_offsets(self) -> None:
        """Replay every real KDBG capture to anchor the KDDEBUGGER_DATA64 map.

        KernBase and PsLoadedModuleList are reachable two independent ways --
        through KdVersion, which the client parses from the version packet, and
        through these offsets -- so agreement across every capture pins the
        struct.  SizePrcb/OffsetPrcbNumber/OffsetPrcbCurrentThread are then the
        target's own statement of its KPRCB layout, and they must match the
        constants PRCB_IPI_FROZEN was generated alongside.
        """
        for directory, block, manifest in CAPTURED_KDBG:
            with self.subTest(harvest=directory.name):
                version = manifest["version"]
                self.assertEqual(
                    struct.unpack_from(
                        "<Q", block, client_module.KDBG_KERN_BASE
                    )[0],
                    version["kernel_base"],
                )
                self.assertEqual(
                    struct.unpack_from(
                        "<Q", block, client_module.KDBG_PS_LOADED_MODULE_LIST
                    )[0],
                    version["loaded_module_list"],
                )
                self.assertEqual(
                    struct.unpack_from(
                        "<H", block, client_module.KDBG_SIZE_PRCB
                    )[0],
                    client_module.PRCB_SIZE,
                )
                self.assertEqual(
                    struct.unpack_from(
                        "<H", block, client_module.KDBG_OFFSET_PRCB_NUMBER
                    )[0],
                    client_module.PRCB_NUMBER,
                )
                self.assertEqual(
                    struct.unpack_from(
                        "<H",
                        block,
                        client_module.KDBG_OFFSET_PRCB_CURRENT_THREAD,
                    )[0],
                    client_module.PRCB_CURRENT_THREAD,
                )
                array = struct.unpack_from(
                    "<Q", block, client_module.KDBG_KI_PROCESSOR_BLOCK
                )[0]
                self.assertGreaterEqual(
                    array, client_module.AMD64_KERNEL_SPACE_START
                )
                self.assertGreaterEqual(array, version["kernel_base"])


class AutomationTests(unittest.TestCase):
    class FakeClient:
        def __init__(
            self,
            harvest_error: Exception | None = None,
            break_error: Exception | None = None,
            harvest_complete: bool = True,
            read_error: Exception | None = None,
            poll_error: Exception | None = None,
        ) -> None:
            self.harvest_error = harvest_error
            self.break_error = break_error
            self.read_error = read_error
            self.poll_error = poll_error
            self.last_harvest_complete = harvest_complete
            self.stopped = False
            self.calls: list[str] = []
            self.events: list[tuple[str, str]] = []

        def _emit(self, kind: str, value: object) -> None:
            self.events.append((kind, str(value)))

        def break_in(self, timeout: float) -> None:
            self.calls.append(f"break:{timeout}")
            if self.break_error is not None:
                raise self.break_error
            self.stopped = True

        def wait_for_stop(self, *, auto_continue_modules: bool, timeout: float) -> None:
            self.calls.append(f"recover-stop:{auto_continue_modules}:{timeout}")
            self.stopped = True

        def harvest(
            self,
            directory: Path,
            *,
            stack_size: int,
            deadline: float | None,
        ) -> Path:
            self.calls.append(f"harvest:{stack_size}")
            if self.harvest_error is not None:
                raise self.harvest_error
            directory.mkdir(parents=True)
            return directory

        def read_virtual(self, address: int, size: int) -> bytes:
            self.calls.append(f"read:0x{address:x}:{size}")
            if self.read_error is not None:
                raise self.read_error
            return bytes((index & 0xFF) for index in range(size))

        @contextmanager
        def operation_deadline(self, _deadline: float):
            yield

        def drain_pending_request(self, timeout: float) -> None:
            self.calls.append(f"drain:{timeout}")

        def continue_execution(self) -> None:
            self.calls.append("continue")
            self.stopped = False

        def poll_idle(self, timeout: float) -> None:
            self.calls.append(f"poll-idle:{timeout}")
            if self.poll_error is not None:
                raise self.poll_error

    def test_read_command_services_the_socket_while_waiting(self) -> None:
        # The REPL waits for its driver far longer than it runs commands.  If
        # that wait does not read the socket, a reboot during it is invisible.
        client = self.FakeClient()
        ready = [False, False, True]

        def fake_select(rlist, _wlist, _xlist, _timeout):
            return (list(rlist) if ready.pop(0) else [], [], [])

        with mock.patch.object(roskd_cli.select, "select", fake_select), mock.patch.object(
            roskd_cli.sys, "stdin", io.StringIO("continue\n")
        ), mock.patch.object(roskd_cli.sys, "stdout", io.StringIO()):
            line = roskd_cli._read_command(client, "roskd> ")

        self.assertEqual(line, "continue\n")
        self.assertEqual(client.calls, ["poll-idle:0.5", "poll-idle:0.5"])
        self.assertEqual(ready, [])

    def test_read_command_propagates_a_target_reboot(self) -> None:
        # KdSessionChanged has to escape the REPL for main() to reconnect.
        client = self.FakeClient(poll_error=KdSessionChanged("new target session"))

        with mock.patch.object(
            roskd_cli.select, "select", lambda *_args: ([], [], [])
        ), mock.patch.object(roskd_cli.sys, "stdout", io.StringIO()):
            with self.assertRaises(KdSessionChanged):
                roskd_cli._read_command(client, "roskd> ")

    def test_repl_lets_a_reboot_reach_the_reconnect_loop(self) -> None:
        # The regression: the REPL used to block in input(), so a target that
        # rebooted while it waited was never noticed and main() never
        # reconnected.  Waiting must surface KdSessionChanged instead.
        client = self.FakeClient(poll_error=KdSessionChanged("new target session"))

        with mock.patch.object(
            roskd_cli.select, "select", lambda *_args: ([], [], [])
        ), mock.patch.object(roskd_cli.sys, "stdin", io.StringIO("")), mock.patch.object(
            roskd_cli.sys, "stdout", io.StringIO()
        ):
            with self.assertRaises(KdSessionChanged):
                roskd_cli.repl(client, False, None, 0x1000, 5.0, 10.0, 5.0)

    def test_idle_repl_wait_detects_a_reboot_over_a_real_socket(self) -> None:
        """End-to-end over loopback UDP: a target reboot during an idle REPL
        wait must be seen, answered, and surfaced as KdSessionChanged.

        This is the failure that stranded a real machine: the listener sat in
        the REPL, the target was power-cycled, and its pokes accumulated unread
        in the socket buffer because nothing in the wait touched the socket.
        """
        port = _free_udp_port()
        target = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.addCleanup(target.close)
        target.bind(("127.0.0.2", port))
        target.connect(("127.0.0.1", port))
        target.settimeout(5.0)

        client = KdNetClient("127.0.0.1", "127.0.0.2", port, "1.2.3.4")
        self.addCleanup(client.close)
        # An established session, as after a successful connect().
        client.client_key = b"a" * 32
        client.host_key = b"h" * 32
        client.crypto.build_response(client.client_key, client.host_key)
        client.stopped = True
        client._open_socket()

        # The rebooted target pokes with a brand new client key.
        reboot_key = b"r" * 32
        target.send(
            KdNetCrypto("1.2.3.4").encode(
                b"\x01\x01" + reboot_key,
                version=1,
                packet_type=KDNET_TYPE_CONTROL,
                sequence=7,
                direction=KDNET_DIRECTION_TARGET,
            )
        )

        # stdin is never ready, so the only way out is through the socket.
        # Fail loudly rather than hang if the wait stops servicing it again.
        polls = []

        def never_ready(*_args):
            polls.append(1)
            if len(polls) > 3:
                raise AssertionError("idle wait never serviced the socket")
            return ([], [], [])

        with mock.patch.object(
            roskd_cli.select, "select", never_ready
        ), mock.patch.object(roskd_cli.sys, "stdout", io.StringIO()):
            with self.assertRaises(KdSessionChanged):
                roskd_cli._read_command(client, "roskd> ")

        # The poke was consumed and answered, so the socket is drained and the
        # client has adopted the new session rather than the stale one.
        reply = KdNetCrypto("1.2.3.4").decode(target.recv(65535))
        self.assertEqual(reply.packet_type, KDNET_TYPE_CONTROL)
        self.assertEqual(reply.payload[:2], b"\x01\x02")
        self.assertEqual(reply.payload[2:34], reboot_key)
        self.assertEqual(client.client_key, reboot_key)

    @staticmethod
    def _stdin(text: str, *, tty: bool) -> io.StringIO:
        """A stdin stand-in whose isatty() answer is under the test's control.

        _read_command branches on it: an empty read is Ctrl-D on a terminal but
        only "no writer attached" on the FIFO the listener is usually driven
        through, and the two paths must be exercised separately.  io.StringIO
        always reports False, so asking for the terminal path needs this.
        """
        stream = io.StringIO(text)
        stream.isatty = lambda: tty
        return stream

    def test_read_command_reports_ctrl_d_on_a_terminal_as_eof(self) -> None:
        client = self.FakeClient()

        with mock.patch.object(
            roskd_cli.select, "select", lambda rlist, *_args: (list(rlist), [], [])
        ), mock.patch.object(
            roskd_cli.sys, "stdin", self._stdin("", tty=True)
        ), mock.patch.object(roskd_cli.sys, "stdout", io.StringIO()):
            with self.assertRaises(EOFError):
                roskd_cli._read_command(client, "roskd> ")

        self.assertEqual(client.calls, [])

    def test_read_command_keeps_polling_when_a_fifo_writer_detaches(self) -> None:
        """700c51fe9fd: an empty read on a FIFO is not EOF.

        Every scripted command closes the pipe, so treating that as EOF ended
        the REPL after one command and stranded the target at its breakpoint.
        The wait must go back to polling the socket instead -- and then pick up
        the next command once a writer reattaches.
        """
        client = self.FakeClient()
        stdin = self._stdin("", tty=False)
        # Two empty reads, as two writers opening and closing the FIFO, then a
        # real command.  Bounded on purpose: if the poll path ever stops
        # advancing, the test fails instead of spinning forever.
        replies = ["", "", "continue\n"]

        def readline() -> str:
            self.assertTrue(replies, "_read_command never returned a command")
            return replies.pop(0)

        stdin.readline = readline

        with mock.patch.object(
            roskd_cli.select, "select", lambda rlist, *_args: (list(rlist), [], [])
        ), mock.patch.object(
            roskd_cli.sys, "stdin", stdin
        ), mock.patch.object(roskd_cli.sys, "stdout", io.StringIO()):
            line = roskd_cli._read_command(client, "roskd> ")

        self.assertEqual(line, "continue\n")
        # Both detached-writer reads went back to servicing the socket rather
        # than ending the session.
        self.assertEqual(client.calls, ["poll-idle:0.5", "poll-idle:0.5"])

    def test_detached_fifo_writer_still_surfaces_a_target_reboot(self) -> None:
        """The non-interactive empty-read path must not swallow a reboot."""
        client = self.FakeClient(poll_error=KdSessionChanged("new target session"))

        with mock.patch.object(
            roskd_cli.select, "select", lambda rlist, *_args: (list(rlist), [], [])
        ), mock.patch.object(
            roskd_cli.sys, "stdin", self._stdin("", tty=False)
        ), mock.patch.object(roskd_cli.sys, "stdout", io.StringIO()):
            with self.assertRaises(KdSessionChanged):
                roskd_cli._read_command(client, "roskd> ")

    def test_stall_capture_owns_break_harvest_and_resume(self) -> None:
        client = self.FakeClient()
        with tempfile.TemporaryDirectory() as temporary:
            roskd_cli._stall_capture(
                client,
                "boot1-token",
                Path(temporary),
                0x4000,
                5.0,
                10.0,
                5.0,
            )

        self.assertEqual(client.calls, ["break:5.0", "harvest:16384", "drain:5.0", "continue"])
        phases = [
            value for kind, value in client.events
            if kind == "transaction"
        ]
        self.assertTrue(any("phase=harvest-finished" in value for value in phases))
        self.assertTrue(any("phase=resume-sent" in value for value in phases))
        self.assertFalse(client.stopped)

    def test_stall_capture_reports_partial_harvest(self) -> None:
        client = self.FakeClient(harvest_complete=False)
        with tempfile.TemporaryDirectory() as temporary:
            roskd_cli._stall_capture(
                client,
                "boot-partial-token",
                Path(temporary),
                0x4000,
                5.0,
                10.0,
                5.0,
            )

        self.assertTrue(
            any(
                kind == "transaction" and "phase=harvest-partial" in value
                for kind, value in client.events
            )
        )
        self.assertFalse(client.stopped)

    def test_stall_capture_recovers_a_late_break_stop(self) -> None:
        client = self.FakeClient(break_error=KdTimeout("break state delayed"))
        with tempfile.TemporaryDirectory() as temporary:
            roskd_cli._stall_capture(
                client,
                "boot-late-token",
                Path(temporary),
                0x4000,
                5.0,
                10.0,
                5.0,
            )

        self.assertEqual(
            client.calls,
            [
                "break:5.0",
                "recover-stop:True:5.0",
                "harvest:16384",
                "drain:5.0",
                "continue",
            ],
        )
        self.assertTrue(
            any(
                kind == "transaction" and "phase=stopped-late" in value
                for kind, value in client.events
            )
        )
        self.assertFalse(client.stopped)

    def test_stall_capture_resumes_after_harvest_failure(self) -> None:
        client = self.FakeClient(KdTimeout("target disappeared"))
        with tempfile.TemporaryDirectory() as temporary:
            roskd_cli._stall_capture(
                client,
                "boot2-token",
                Path(temporary),
                0x4000,
                5.0,
                10.0,
                5.0,
            )

        self.assertEqual(client.calls, ["break:5.0", "harvest:16384", "drain:5.0", "continue"])
        self.assertIn(("harvest-failed", "target disappeared"), client.events)
        self.assertTrue(
            any(
                kind == "transaction" and "phase=resume-sent" in value
                for kind, value in client.events
            )
        )
        self.assertFalse(client.stopped)

    def test_bounded_peek_reads_and_resumes_before_writing(self) -> None:
        client = self.FakeClient()
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "peek.bin"
            data = roskd_cli._bounded_peek(
                client,
                0xFFFFF80000674128,
                16,
                destination,
                5.0,
                10.0,
                5.0,
            )
            self.assertEqual(destination.read_bytes(), bytes(range(16)))

        self.assertEqual(data, bytes(range(16)))
        self.assertEqual(
            client.calls,
            [
                "break:5.0",
                "read:0xfffff80000674128:16",
                "drain:5.0",
                "continue",
            ],
        )
        self.assertTrue(
            any(
                kind == "transaction" and "phase=resume-sent" in value
                for kind, value in client.events
            )
        )
        self.assertFalse(client.stopped)

    def test_bounded_peek_resumes_after_read_failure(self) -> None:
        client = self.FakeClient(read_error=KdTimeout("read disappeared"))
        with self.assertRaisesRegex(KdTimeout, "read disappeared"):
            roskd_cli._bounded_peek(
                client,
                0xFFFFF80000674128,
                16,
                None,
                5.0,
                10.0,
                5.0,
            )

        self.assertEqual(
            client.calls,
            [
                "break:5.0",
                "read:0xfffff80000674128:16",
                "drain:5.0",
                "continue",
            ],
        )
        self.assertTrue(
            any(
                kind == "transaction" and "phase=capture-failed" in value
                for kind, value in client.events
            )
        )
        self.assertFalse(client.stopped)

    def test_bounded_peek_rejects_unbounded_size(self) -> None:
        client = self.FakeClient()
        for size in (0, 0x10001):
            with self.subTest(size=size), self.assertRaises(ValueError):
                roskd_cli._bounded_peek(
                    client,
                    0xFFFFF80000674128,
                    size,
                    None,
                    5.0,
                    10.0,
                    5.0,
                )
        self.assertEqual(client.calls, [])

    def test_harvest_deadline_writes_partial_manifest(self) -> None:
        client = object.__new__(KdNetClient)
        client.stopped = True
        client.current_state = KdStateChange(
            state=DBGKD_EXCEPTION_STATE_CHANGE,
            cpu_level=6,
            cpu=0,
            cpu_count=1,
            thread=0x3000,
            program_counter=0x1100,
        )
        client.version = KdVersion(12, 3790, 6, 2, 7, 0x8664, 0x1000, 0x4000, 0x5000)
        client.last_state_payload = b"state"
        client.observed_modules = {}
        client._operation_deadline = None
        client._log = None
        events: list[tuple[str, object]] = []
        client.event_callback = lambda kind, value: events.append((kind, value))

        def get_context(_processor: int | None = None) -> bytes:
            raise KdTimeout("capture deadline expired")

        client.get_context = get_context
        with tempfile.TemporaryDirectory() as temporary:
            destination = client.harvest(
                Path(temporary), deadline=time.monotonic() + 1.0
            )
            manifest = json.loads((destination / "manifest.json").read_text())

        self.assertFalse(manifest["complete"])
        self.assertEqual(manifest["incomplete_reason"], "capture deadline expired")
        self.assertEqual(events[-1][0], "harvest-partial")


if __name__ == "__main__":
    unittest.main()
