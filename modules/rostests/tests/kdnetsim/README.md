# KDNET target simulator

`kdnetsim` exercises the ReactOS KDNET and inner KD protocol cores against
Radare2 without booting an operating system. It behaves like a minimal AMD64
target through:

- the authenticated KDNET control handshake;
- the debugger's one-byte break-in request;
- a `StateChange64` packet and its acknowledgement;
- a `DbgKdGetVersionApi` request, acknowledgement, and response; and
- the debugger's first `DbgKdReadVirtualMemoryApi` request and acknowledgement.

Passing the optional `full` argument keeps a small in-memory AMD64 fake target
alive after version discovery. In that mode it supports:

- virtual-memory reads and persistent writes;
- `GetContext` and `SetContext` with a fake AMD64 context;
- software-breakpoint insertion and restoration;
- `Continue` and `Continue2`; and
- one deliberately injected `RESEND` before the first write, plus response
  retransmission when the debugger requests it.

Radare2 binds its local UDP endpoint to the requested port. For a same-machine
test, use two addresses from the loopback network:

```text
r2 -d winkd://127.0.0.2:50000:1.2.3.4
kdnetsim.exe 127.0.0.2 127.0.0.1 50000 1.2.3.4
```

Use `... 1.2.3.4 full` for the extended fake-target mode. A successful full
run ends after the consumer sends continue:

```text
Continue received; full fake-target session completed.
```

Start Radare2 first. A successful run ends with:

```text
Inner KD synchronized; GetVersion replied; ReadVirtualMemory received.
```

Without `full`, the simulator exits after acknowledging the first memory
request, preserving the short compatibility test used for a normal debugger
attach. The fake target is deliberately synthetic and is not a Windows kernel
image, so consumers that require kernel symbols or real process structures
still cannot finish those higher-level discovery operations.

The interoperability run was verified with Radare2 commit
`476efd4d2fb56a28ecce6a9fc48db5a13a4a1310`, whose KDNET backend calls its
HMAC-SHA-256 implementation directly. Radare2 6.1.4 cannot currently be used
for this test: its KDNET backend requests an `hmac-sha256` RMuta algorithm,
but that algorithm is not registered, so it rejects every Poke as an
authentication failure. This is a Radare2 regression rather than a different
KDNET wire format.

The same exchange was verified with Rizin commit
`bceffc3e9ff79f57760899abaaf98ec58fd73dda`. Rizin may acknowledge the
initial unused KD packet before sending its break-in request; it also preserves
the sync bit when acknowledging the initial state change and acknowledges
control ACK packets. The simulator accepts those extra acknowledgements as
well as Radare2's behavior.

That Rizin revision leaves its UDP socket nonblocking while `rzwinkd` treats an
initial `EAGAIN` as a malformed packet. The interoperability run used a local
one-line consumer fix that calls `rz_socket_block_time(sock, true, 0, 0)` after
`rz_socket_connect_udp`. No KDNET or inner KD wire behavior was changed in
Rizin for the test.

The client key is deterministic because this tool tests interoperability. A
real transport must obtain it from a suitable random source.
