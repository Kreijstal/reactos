"""KDNET protection and inner Microsoft KD packet framing.

The implementation mirrors the allocation-free C implementation in
``sdk/lib/kdnet`` and deliberately keeps transport framing separate from the
debugger session in :mod:`roskd.client`.
"""

from __future__ import annotations

import hashlib
import hmac
import struct
from dataclasses import dataclass

try:
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
except ImportError as error:  # pragma: no cover - exercised by the CLI error path
    raise ImportError(
        "roskd requires the 'cryptography' Python package"
    ) from error


KDNET_MAGIC = b"MDBG"
KDNET_HEADER_SIZE = 6
KDNET_DATA_SIZE = 8
KDNET_AUTH_SIZE = 16
KDNET_TYPE_DATA = 0
KDNET_TYPE_CONTROL = 1
KDNET_DIRECTION_TARGET = 0
KDNET_DIRECTION_DEBUGGER = 8

KD_LEADER_UNUSED = 0x00000000
KD_LEADER_DATA = 0x30303030
KD_LEADER_CONTROL = 0x69696969
KD_HEADER_SIZE = 16
KD_UNUSED_SIZE = 12
KD_MAX_PAYLOAD = 4000
KD_INITIAL_ID = 0x80800000
KD_SYNC_ID = 0x00000800

KD_TYPE_UNUSED = 0
KD_TYPE_STATE_CHANGE32 = 1
KD_TYPE_STATE_MANIPULATE = 2
KD_TYPE_DEBUG_IO = 3
KD_TYPE_ACKNOWLEDGE = 4
KD_TYPE_RESEND = 5
KD_TYPE_RESET = 6
KD_TYPE_STATE_CHANGE64 = 7
KD_TYPE_POLL_BREAKIN = 8
KD_TYPE_TRACE_IO = 9
KD_TYPE_CONTROL_REQUEST = 10
KD_TYPE_FILE_IO = 11
KD_TYPE_MAX = 12


class ProtocolError(ValueError):
    """A malformed, unauthenticated, or out-of-sequence packet was received."""


@dataclass(frozen=True)
class KdNetPacket:
    version: int
    packet_type: int
    direction: int
    sequence: int
    payload: bytes
    padding: int = 0


@dataclass(frozen=True)
class KdPacket:
    leader: int
    packet_type: int
    packet_id: int
    payload: bytes = b""
    checksum: int = 0

    @property
    def is_data(self) -> bool:
        return self.leader == KD_LEADER_DATA

    @property
    def is_control(self) -> bool:
        return self.leader == KD_LEADER_CONTROL


def _decode_key(key_string: str) -> bytes:
    parts = key_string.split(".")
    if len(parts) != 4:
        raise ProtocolError("KDNET key must contain four base-36 components")

    decoded = bytearray()
    for part in parts:
        if not part or len(part) > 13:
            raise ProtocolError("invalid KDNET key component")
        if any(not (character.isascii() and character.isalnum()) for character in part):
            raise ProtocolError("invalid base-36 digit in KDNET key")
        try:
            value = int(part, 36)
        except ValueError as error:
            raise ProtocolError("invalid base-36 digit in KDNET key") from error
        if value > 0xFFFFFFFFFFFFFFFF:
            raise ProtocolError("KDNET key component exceeds 64 bits")
        decoded.extend(value.to_bytes(8, "little"))
    return bytes(decoded)


class KdNetCrypto:
    """Encode and decode authenticated KDNET UDP payloads."""

    def __init__(self, key_string: str):
        self.control_key = _decode_key(key_string)
        self.hmac_key = bytes((~value) & 0xFF for value in self.control_key)
        self.data_key: bytes | None = None

    def derive_data_key(self, response_payload: bytes) -> bytes:
        self.data_key = hashlib.sha256(
            self.control_key + response_payload
        ).digest()
        return self.data_key

    def build_response(
        self,
        client_key: bytes,
        host_key: bytes,
    ) -> bytes:
        if len(client_key) != 32 or len(host_key) != 32:
            raise ProtocolError("KDNET client and host keys must be 32 bytes")
        response = b"\x01\x02" + client_key + host_key + bytes(256)
        self.derive_data_key(response)
        return response

    def _key_for(self, packet_type: int) -> bytes:
        if packet_type == KDNET_TYPE_CONTROL:
            return self.control_key
        if packet_type == KDNET_TYPE_DATA:
            if self.data_key is None:
                raise ProtocolError("KDNET data key is not established")
            return self.data_key
        raise ProtocolError(f"unknown KDNET packet type {packet_type}")

    def encode(
        self,
        payload: bytes,
        *,
        version: int,
        packet_type: int,
        sequence: int,
        direction: int,
    ) -> bytes:
        if not 0 <= version <= 0xFF:
            raise ProtocolError("KDNET version does not fit in one byte")
        if direction not in (KDNET_DIRECTION_TARGET, KDNET_DIRECTION_DEBUGGER):
            raise ProtocolError("invalid KDNET direction")
        if not 0 <= sequence <= 0x00FFFFFFFFFFFFFF:
            raise ProtocolError("KDNET sequence exceeds 56 bits")

        header = KDNET_MAGIC + bytes((version, packet_type))
        padding = (-(KDNET_DATA_SIZE + len(payload))) & 0x0F
        packet_data = (
            (sequence << 8) | (direction << 4) | padding
        ).to_bytes(8, "big")
        plaintext = packet_data + payload + bytes(padding)
        tag = hmac.new(
            self.hmac_key, header + plaintext, hashlib.sha256
        ).digest()[:KDNET_AUTH_SIZE]
        encryptor = Cipher(
            algorithms.AES(self._key_for(packet_type)), modes.CBC(tag)
        ).encryptor()
        ciphertext = encryptor.update(plaintext) + encryptor.finalize()
        return header + ciphertext + tag

    def decode(self, packet: bytes) -> KdNetPacket:
        minimum = KDNET_HEADER_SIZE + 16 + KDNET_AUTH_SIZE
        if len(packet) < minimum or packet[:4] != KDNET_MAGIC:
            raise ProtocolError("invalid KDNET packet header")
        version = packet[4]
        packet_type = packet[5]
        encrypted = packet[KDNET_HEADER_SIZE:-KDNET_AUTH_SIZE]
        tag = packet[-KDNET_AUTH_SIZE:]
        if len(encrypted) % 16:
            raise ProtocolError("misaligned KDNET protected payload")

        decryptor = Cipher(
            algorithms.AES(self._key_for(packet_type)), modes.CBC(tag)
        ).decryptor()
        plaintext = decryptor.update(encrypted) + decryptor.finalize()
        expected = hmac.new(
            self.hmac_key,
            packet[:KDNET_HEADER_SIZE] + plaintext,
            hashlib.sha256,
        ).digest()[:KDNET_AUTH_SIZE]
        if not hmac.compare_digest(expected, tag):
            raise ProtocolError("KDNET authentication failed")

        packet_data = int.from_bytes(plaintext[:KDNET_DATA_SIZE], "big")
        padding = packet_data & 0x0F
        direction = (packet_data >> 4) & 0x0F
        sequence = packet_data >> 8
        if direction not in (KDNET_DIRECTION_TARGET, KDNET_DIRECTION_DEBUGGER):
            raise ProtocolError("invalid KDNET packet direction")
        if padding > len(plaintext) - KDNET_DATA_SIZE:
            raise ProtocolError("invalid KDNET padding length")
        if padding and any(plaintext[-padding:]):
            raise ProtocolError("non-zero KDNET padding")
        end = len(plaintext) - padding if padding else len(plaintext)
        return KdNetPacket(
            version=version,
            packet_type=packet_type,
            direction=direction,
            sequence=sequence,
            payload=plaintext[KDNET_DATA_SIZE:end],
            padding=padding,
        )


def kd_checksum(payload: bytes) -> int:
    return sum(payload) & 0xFFFFFFFF


def encode_kd_data(packet_type: int, packet_id: int, payload: bytes) -> bytes:
    if not 0 < packet_type < KD_TYPE_MAX:
        raise ProtocolError("invalid inner KD data type")
    if len(payload) > KD_MAX_PAYLOAD:
        raise ProtocolError("inner KD payload is too large")
    return struct.pack(
        "<IHHII",
        KD_LEADER_DATA,
        packet_type,
        len(payload),
        packet_id & 0xFFFFFFFF,
        kd_checksum(payload),
    ) + payload


def encode_kd_control(packet_type: int, packet_id: int) -> bytes:
    if packet_type not in (KD_TYPE_ACKNOWLEDGE, KD_TYPE_RESEND, KD_TYPE_RESET):
        raise ProtocolError("invalid inner KD control type")
    return struct.pack(
        "<IHHII",
        KD_LEADER_CONTROL,
        packet_type,
        0,
        packet_id & 0xFFFFFFFF,
        0,
    )


def decode_kd_packet(packet: bytes) -> KdPacket:
    if packet == bytes(KD_UNUSED_SIZE):
        return KdPacket(KD_LEADER_UNUSED, KD_TYPE_UNUSED, 0)
    if len(packet) < KD_HEADER_SIZE:
        raise ProtocolError("truncated inner KD packet")

    leader, packet_type, length, packet_id, checksum = struct.unpack_from(
        "<IHHII", packet
    )
    if length > KD_MAX_PAYLOAD or len(packet) != KD_HEADER_SIZE + length:
        raise ProtocolError("invalid inner KD packet length")
    payload = packet[KD_HEADER_SIZE:]
    if leader == KD_LEADER_CONTROL:
        if packet_type not in (
            KD_TYPE_ACKNOWLEDGE,
            KD_TYPE_RESEND,
            KD_TYPE_RESET,
        ) or length or checksum:
            raise ProtocolError("invalid inner KD control packet")
    elif leader == KD_LEADER_DATA:
        if not 0 < packet_type < KD_TYPE_MAX:
            raise ProtocolError("invalid inner KD data packet type")
        if checksum != kd_checksum(payload):
            raise ProtocolError("inner KD checksum mismatch")
    else:
        raise ProtocolError("invalid inner KD packet leader")
    return KdPacket(leader, packet_type, packet_id, payload, checksum)
