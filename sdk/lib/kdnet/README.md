# KDNET protocol core

This library implements the allocation-free KDNET cryptographic envelope and
the inner KD packet framing carried by its encrypted UDP datagrams. It
provides:

- four-part base-36 control-key decoding;
- target Poke construction, host Response validation, and session-key setup;
- HMAC-key and session data-key derivation;
- truncated HMAC-SHA-256 authentication;
- AES-256-CBC packet protection;
- KDNET header, sequence, direction, and padding handling;
- KD data and control packet encoding and validation;
- KD byte checksums, packet IDs, and payload limits;
- reliable KD session state for ACK, RESET, RESEND, duplicate suppression,
  and bounded timeout retries; and
- KDNET's special checksum-less initial unused packet.

The wire behavior was independently implemented from protocol observations
and interoperability research in radare2's host-side WinKD backend. The code
here is original ReactOS code and does not incorporate radare2 source.

This is deliberately separate from Ethernet, IPv4, UDP, and NIC access. A
target transport can call it with fixed caller-owned buffers while interrupts
and allocation are unavailable. Higher-level debugger requests are already
handled by ReactOS under `ntoskrnl/kd64`; a KDNET transport can bridge that
existing `KdSendPacket`/`KdReceivePacket` interface to these codecs.

The session helper does not own packet storage or a timer. Its caller retains
the last encoded packet and calls `KdPacketSessionTimeout` when its transport
deadline expires. This keeps retransmission policy usable in early boot and
interrupt contexts without allocation or platform-specific timing APIs.

The `kdnetsim` test peer under `modules/rostests/tests` exercises the control
handshake and an inner KD exchange through state change, version discovery,
and the first virtual-memory request against Radare2 over UDP, without booting
ReactOS.
