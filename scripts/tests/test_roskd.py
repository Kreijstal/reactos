#!/usr/bin/env python3
"""Focused compatibility tests for the native Python KDNET client."""

from __future__ import annotations

import hashlib
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))

from roskd.client import (  # noqa: E402
    DBGKD_EXCEPTION_STATE_CHANGE,
    DBGKD_GET_STRING_API,
    DBGKD_LOAD_SYMBOLS_STATE_CHANGE,
    KdModule,
    KdNetClient,
    KdRequestError,
    KdStateChange,
    KdVersion,
    parse_amd64_context,
    parse_amd64_loader_entry,
    parse_state_change,
)
from roskd.protocol import (  # noqa: E402
    KD_INITIAL_ID,
    KD_LEADER_CONTROL,
    KD_LEADER_DATA,
    KD_SYNC_ID,
    KD_TYPE_ACKNOWLEDGE,
    KD_TYPE_DEBUG_IO,
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
    def test_manual_break_resume_advances_rewound_amd64_rip(self) -> None:
        client = object.__new__(KdNetClient)
        client._manual_break_stop = True
        client.current_state = KdStateChange(
            state=DBGKD_EXCEPTION_STATE_CHANGE,
            cpu_level=6,
            cpu=0,
            cpu_count=4,
            thread=0x1234,
            program_counter=0xFFFFF80000102030,
            exception_code=0x80000003,
            exception_address=0xFFFFF80000102031,
        )
        client.version = KdVersion(12, 3790, 6, 2, 7, 0x8664, 0, 0, 0)
        context = bytearray(0x4D0)
        struct.pack_into("<Q", context, 0xF8, client.current_state.program_counter)
        written = []
        events = []
        client.get_context = lambda: bytes(context)
        client.set_context = written.append
        client._emit = lambda kind, value: events.append((kind, value))

        client._prepare_manual_break_resume()

        self.assertFalse(client._manual_break_stop)
        self.assertEqual(len(written), 1)
        self.assertEqual(
            struct.unpack_from("<Q", written[0], 0xF8)[0],
            client.current_state.exception_address,
        )
        self.assertEqual(events[0][0], "resume-fixup")

    def test_regular_breakpoint_resume_does_not_change_context(self) -> None:
        client = object.__new__(KdNetClient)
        client._manual_break_stop = True
        client.current_state = KdStateChange(
            state=DBGKD_EXCEPTION_STATE_CHANGE,
            cpu_level=6,
            cpu=0,
            cpu_count=4,
            thread=0x1234,
            program_counter=0xFFFFF80000102030,
            exception_code=0x80000003,
            exception_address=0xFFFFF80000102030,
        )
        client.version = KdVersion(12, 3790, 6, 2, 7, 0x8664, 0, 0, 0)
        client.get_context = lambda: self.fail("regular breakpoint context read")

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


if __name__ == "__main__":
    unittest.main()
