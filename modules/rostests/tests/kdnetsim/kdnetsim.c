/*
 * PROJECT:     ReactOS tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     User-mode KDNET target simulator for Radare2 interoperability
 */

#define WIN32_LEAN_AND_MEAN
#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <ndk/ntndk.h>

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>

#include <reactos/kdprotocol.h>
#include <reactos/kdnetprotocol.h>
#include <reactos/windbgkd.h>

#define SIMULATOR_PACKET_CAPACITY 4096
#define SIMULATOR_TIMEOUT_MS      10000
#define SIMULATOR_MEMORY_SIZE     0x4000
#define SIMULATOR_CONTEXT_SIZE    0x4d0
#define SIMULATOR_BREAKPOINTS     16
#define SIMULATOR_MEMORY_BASE     0xfffff80000000000ULL

static uint32_t
ReadLittleEndian32(const unsigned char *Buffer)
{
    return (uint32_t)Buffer[0] |
           ((uint32_t)Buffer[1] << 8) |
           ((uint32_t)Buffer[2] << 16) |
           ((uint32_t)Buffer[3] << 24);
}

static void
PrintUsage(const char *Program)
{
    fprintf(stderr,
            "Usage: %s <local-target-ip> <debugger-ip> <port> <key> [full]\n"
            "\n"
            "Same-machine example:\n"
            "  Radare2:  r2 -d winkd://127.0.0.2:50000:1.2.3.4\n"
            "  Target:   %s 127.0.0.2 127.0.0.1 50000 1.2.3.4\n",
            Program,
            Program);
}

static int
ParseAddress(const char *Text, unsigned short Port, struct sockaddr_in *Address)
{
    /* inet_pton is a ws2_32 export only from Vista on (-version=0x600+), and
     * this only ever parses a dotted-quad, which inet_addr has always done. */
    unsigned long Parsed = inet_addr(Text);

    if (Parsed == INADDR_NONE)
        return 0;

    Address->sin_family = AF_INET;
    Address->sin_port = htons(Port);
    Address->sin_addr.s_addr = Parsed;
    return 1;
}

static int
SendPacket(SOCKET Socket, const unsigned char *Packet, size_t PacketLength)
{
    int Sent;

    if (PacketLength > INT_MAX)
        return 0;
    Sent = send(Socket, (const char *)Packet, (int)PacketLength, 0);
    if (Sent == SOCKET_ERROR)
    {
        fprintf(stderr, "send failed: Winsock error %d\n", WSAGetLastError());
        return 0;
    }
    if ((size_t)Sent != PacketLength)
    {
        fprintf(stderr, "short UDP send: %d of %Iu bytes\n", Sent, PacketLength);
        return 0;
    }
    return 1;
}

static int
ReceivePacket(SOCKET Socket, unsigned char *Packet, size_t Capacity, size_t *PacketLength)
{
    int Received;

    if (Capacity > INT_MAX)
        Capacity = INT_MAX;
    Received = recv(Socket, (char *)Packet, (int)Capacity, 0);
    if (Received == SOCKET_ERROR)
    {
        fprintf(stderr, "receive failed: Winsock error %d\n", WSAGetLastError());
        return 0;
    }
    *PacketLength = (size_t)Received;
    return 1;
}

int
main(int ArgumentCount, char **Arguments)
{
    WSADATA WsaData;
    KDNET_CRYPTO_CONTEXT CryptoContext;
    KDNET_PACKET_INFO PacketInfo;
    KDNET_STATUS Status;
    KD_PACKET_STATUS KdStatus;
    KD_PACKET_VIEW KdPacketView;
    DBGKD_ANY_WAIT_STATE_CHANGE StateChange;
    DBGKD_MANIPULATE_STATE64 ManipulateRequest;
    DBGKD_MANIPULATE_STATE64 VersionResponse;
    DBGKD_MANIPULATE_STATE64 ManipulateResponse;
    struct sockaddr_in LocalAddress, DebuggerAddress;
    unsigned char ClientKey[KDNET_CLIENT_KEY_SIZE];
    unsigned char Poke[KDNET_CONTROL_POKE_SIZE];
    unsigned char Packet[SIMULATOR_PACKET_CAPACITY];
    unsigned char KdPacket[SIMULATOR_PACKET_CAPACITY];
    unsigned char LastKdPacket[SIMULATOR_PACKET_CAPACITY];
    unsigned char TargetMemory[SIMULATOR_MEMORY_SIZE];
    unsigned char ContextData[SIMULATOR_CONTEXT_SIZE];
    unsigned long long BreakpointAddresses[SIMULATOR_BREAKPOINTS];
    unsigned long ParsedPort;
    unsigned long Timeout = SIMULATOR_TIMEOUT_MS;
    size_t PokeLength, PacketLength, KdPacketLength, LastKdPacketLength, Index;
    size_t ResponseDataLength, MemoryOffset, RequestDataLength;
    const unsigned char *ResponseData;
    const unsigned char *RequestData;
    unsigned long TargetSequence = 6;
    unsigned long TargetPacketId = KD_PACKET_INITIAL_ID;
    unsigned long BreakpointHandle;
    unsigned long RetryCount;
    unsigned long long DebuggerDataAddress = SIMULATOR_MEMORY_BASE + 0x2000;
    SOCKET Socket = INVALID_SOCKET;
    int FullMode;
    int ResendInjected = 0;
    int Result = 1;

    if (ArgumentCount != 5 && ArgumentCount != 6)
    {
        PrintUsage(Arguments[0]);
        return 2;
    }
    FullMode = ArgumentCount == 6 && strcmp(Arguments[5], "full") == 0;
    if (ArgumentCount == 6 && !FullMode)
    {
        PrintUsage(Arguments[0]);
        return 2;
    }

    ParsedPort = strtoul(Arguments[3], NULL, 10);
    if (ParsedPort == 0 || ParsedPort > 65535)
    {
        fprintf(stderr, "invalid UDP port: %s\n", Arguments[3]);
        return 2;
    }

    if (!ParseAddress(Arguments[1], (unsigned short)ParsedPort, &LocalAddress) ||
        !ParseAddress(Arguments[2], (unsigned short)ParsedPort, &DebuggerAddress))
    {
        fprintf(stderr, "the simulator currently requires numeric IPv4 addresses\n");
        return 2;
    }

    Status = KdNetInitializeCryptoContext(&CryptoContext, Arguments[4]);
    if (Status != KdNetStatusSuccess)
    {
        fprintf(stderr, "invalid KDNET key (status %d)\n", Status);
        return 2;
    }

    /* Deterministic is intentional: this is an interoperability test peer. */
    for (Index = 0; Index < sizeof(ClientKey); ++Index)
        ClientKey[Index] = (unsigned char)Index;

    ZeroMemory(TargetMemory, sizeof(TargetMemory));
    ZeroMemory(ContextData, sizeof(ContextData));
    ZeroMemory(BreakpointAddresses, sizeof(BreakpointAddresses));
    CopyMemory(TargetMemory + 0x1000,
               &DebuggerDataAddress,
               sizeof(DebuggerDataAddress));

    Status = KdNetBuildPokePayload(ClientKey,
                                   Poke,
                                   sizeof(Poke),
                                   &PokeLength);
    if (Status != KdNetStatusSuccess)
    {
        fprintf(stderr, "could not construct Poke (status %d)\n", Status);
        return 1;
    }

    if (WSAStartup(MAKEWORD(2, 2), &WsaData) != 0)
    {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (Socket == INVALID_SOCKET)
    {
        fprintf(stderr, "socket failed: Winsock error %d\n", WSAGetLastError());
        goto Cleanup;
    }

    if (setsockopt(Socket,
                   SOL_SOCKET,
                   SO_RCVTIMEO,
                   (const char *)&Timeout,
                   sizeof(Timeout)) == SOCKET_ERROR)
    {
        fprintf(stderr, "SO_RCVTIMEO failed: Winsock error %d\n", WSAGetLastError());
        goto Cleanup;
    }

    if (bind(Socket,
             (const struct sockaddr *)&LocalAddress,
             sizeof(LocalAddress)) == SOCKET_ERROR)
    {
        fprintf(stderr,
                "bind(%s:%lu) failed: Winsock error %d\n",
                Arguments[1],
                ParsedPort,
                WSAGetLastError());
        goto Cleanup;
    }

    if (connect(Socket,
                (const struct sockaddr *)&DebuggerAddress,
                sizeof(DebuggerAddress)) == SOCKET_ERROR)
    {
        fprintf(stderr,
                "connect(%s:%lu) failed: Winsock error %d\n",
                Arguments[2],
                ParsedPort,
                WSAGetLastError());
        goto Cleanup;
    }

    Status = KdNetEncodePacket(&CryptoContext,
                               1,
                               KDNET_PACKET_TYPE_CONTROL,
                               1,
                               KDNET_DIRECTION_TARGET,
                               Poke,
                               PokeLength,
                               Packet,
                               sizeof(Packet),
                               &PacketLength);
    if (Status != KdNetStatusSuccess)
    {
        fprintf(stderr, "could not encode Poke (status %d)\n", Status);
        goto Cleanup;
    }

    printf("Sending KDNET Poke from %s:%lu to %s:%lu...\n",
           Arguments[1], ParsedPort, Arguments[2], ParsedPort);
    if (!SendPacket(Socket, Packet, PacketLength))
        goto Cleanup;

    if (!ReceivePacket(Socket, Packet, sizeof(Packet), &PacketLength))
        goto Cleanup;
    Status = KdNetDecodePacket(&CryptoContext,
                               Packet,
                               PacketLength,
                               &PacketInfo);
    if (Status != KdNetStatusSuccess)
    {
        fprintf(stderr, "could not decode Response (status %d)\n", Status);
        goto Cleanup;
    }
    if (PacketInfo.Type != KDNET_PACKET_TYPE_CONTROL ||
        PacketInfo.Direction != KDNET_DIRECTION_DEBUGGER ||
        PacketInfo.SequenceNumber != 1)
    {
        fprintf(stderr, "unexpected KDNET Response metadata\n");
        goto Cleanup;
    }

    Status = KdNetProcessResponsePayload(&CryptoContext,
                                         ClientKey,
                                         PacketInfo.Payload,
                                         PacketInfo.PayloadLength);
    if (Status != KdNetStatusSuccess)
    {
        fprintf(stderr, "invalid KDNET Response (status %d)\n", Status);
        goto Cleanup;
    }
    printf("Control handshake authenticated; session data key established.\n");

    KdStatus = KdPacketEncodeUnused(KdPacket,
                                    sizeof(KdPacket),
                                    &KdPacketLength);
    if (KdStatus != KdPacketStatusSuccess)
    {
        fprintf(stderr, "could not encode initial unused KD packet (status %d)\n",
                KdStatus);
        goto Cleanup;
    }

    /* KDNET consumers read one checksum-less unused KD packet before break-in. */
    Status = KdNetEncodePacket(&CryptoContext,
                               1,
                               KDNET_PACKET_TYPE_DATA,
                               2,
                               KDNET_DIRECTION_TARGET,
                               KdPacket,
                               KdPacketLength,
                               Packet,
                               sizeof(Packet),
                               &PacketLength);
    if (Status != KdNetStatusSuccess)
    {
        fprintf(stderr, "could not encode initial KD packet (status %d)\n", Status);
        goto Cleanup;
    }
    if (!SendPacket(Socket, Packet, PacketLength))
        goto Cleanup;

    if (!ReceivePacket(Socket, Packet, sizeof(Packet), &PacketLength))
        goto Cleanup;
    Status = KdNetDecodePacket(&CryptoContext,
                               Packet,
                               PacketLength,
                               &PacketInfo);
    if (Status != KdNetStatusSuccess)
    {
        fprintf(stderr, "could not decode debugger packet (status %d)\n", Status);
        goto Cleanup;
    }

    /* Rizin acknowledges the initial unused packet; Radare2 goes straight to break-in. */
    if (PacketInfo.Type == KDNET_PACKET_TYPE_DATA &&
        PacketInfo.Direction == KDNET_DIRECTION_DEBUGGER &&
        KdPacketDecode(PacketInfo.Payload,
                       PacketInfo.PayloadLength,
                       &KdPacketView) == KdPacketStatusSuccess &&
        KdPacketView.Leader == KD_PACKET_LEADER_CONTROL &&
        KdPacketView.Type == KD_PACKET_TYPE_ACKNOWLEDGE &&
        KdPacketView.Id == 0)
    {
        if (!ReceivePacket(Socket, Packet, sizeof(Packet), &PacketLength))
            goto Cleanup;
        Status = KdNetDecodePacket(&CryptoContext,
                                   Packet,
                                   PacketLength,
                                   &PacketInfo);
        if (Status != KdNetStatusSuccess)
        {
            fprintf(stderr, "could not decode post-unused debugger packet (status %d)\n",
                    Status);
            goto Cleanup;
        }
    }

    if (PacketInfo.Type != KDNET_PACKET_TYPE_DATA ||
        PacketInfo.Direction != KDNET_DIRECTION_DEBUGGER ||
        PacketInfo.PayloadLength != 1 || PacketInfo.Payload[0] != 'b')
    {
        fprintf(stderr, "expected the debugger's one-byte break-in packet\n");
        goto Cleanup;
    }

    printf("Debugger break-in received: end-to-end KDNET transport succeeded.\n");

    ZeroMemory(&StateChange, sizeof(StateChange));
    StateChange.NewState = DbgKdExceptionStateChange;
    StateChange.ProcessorLevel = 6;
    StateChange.Processor = 0;
    StateChange.NumberProcessors = 1;
    StateChange.Thread = 0x1000;
    StateChange.ProgramCounter = 0x1000;
    StateChange.u.Exception.ExceptionRecord.ExceptionCode = 0x80000003;
    StateChange.u.Exception.ExceptionRecord.ExceptionAddress = 0x1000;
    StateChange.u.Exception.FirstChance = 1;

    KdStatus = KdPacketEncodeData(KD_PACKET_TYPE_STATE_CHANGE64,
                                  KD_PACKET_INITIAL_ID | KD_PACKET_SYNC_ID,
                                  (const unsigned char *)&StateChange,
                                  sizeof(StateChange),
                                  NULL,
                                  0,
                                  KdPacket,
                                  sizeof(KdPacket),
                                  &KdPacketLength);
    if (KdStatus != KdPacketStatusSuccess)
    {
        fprintf(stderr, "could not encode KD state change (status %d)\n", KdStatus);
        goto Cleanup;
    }

    Status = KdNetEncodePacket(&CryptoContext,
                               1,
                               KDNET_PACKET_TYPE_DATA,
                               3,
                               KDNET_DIRECTION_TARGET,
                               KdPacket,
                               KdPacketLength,
                               Packet,
                               sizeof(Packet),
                               &PacketLength);
    if (Status != KdNetStatusSuccess ||
        !SendPacket(Socket, Packet, PacketLength))
    {
        fprintf(stderr, "could not send KD state change (status %d)\n", Status);
        goto Cleanup;
    }

    if (!ReceivePacket(Socket, Packet, sizeof(Packet), &PacketLength))
        goto Cleanup;
    Status = KdNetDecodePacket(&CryptoContext,
                               Packet,
                               PacketLength,
                               &PacketInfo);
    if (Status != KdNetStatusSuccess ||
        PacketInfo.Type != KDNET_PACKET_TYPE_DATA ||
        PacketInfo.Direction != KDNET_DIRECTION_DEBUGGER)
    {
        fprintf(stderr, "could not decode state-change ACK (status %d)\n", Status);
        goto Cleanup;
    }
    KdStatus = KdPacketDecode(PacketInfo.Payload,
                              PacketInfo.PayloadLength,
                              &KdPacketView);
    if (KdStatus != KdPacketStatusSuccess ||
        KdPacketView.Leader != KD_PACKET_LEADER_CONTROL ||
        KdPacketView.Type != KD_PACKET_TYPE_ACKNOWLEDGE ||
        (KdPacketView.Id != KD_PACKET_INITIAL_ID &&
         KdPacketView.Id != (KD_PACKET_INITIAL_ID | KD_PACKET_SYNC_ID)))
    {
        fprintf(stderr, "invalid state-change ACK (status %d)\n", KdStatus);
        goto Cleanup;
    }

    if (!ReceivePacket(Socket, Packet, sizeof(Packet), &PacketLength))
        goto Cleanup;
    Status = KdNetDecodePacket(&CryptoContext,
                               Packet,
                               PacketLength,
                               &PacketInfo);
    if (Status != KdNetStatusSuccess ||
        PacketInfo.Type != KDNET_PACKET_TYPE_DATA ||
        PacketInfo.Direction != KDNET_DIRECTION_DEBUGGER)
    {
        fprintf(stderr, "could not decode debugger KD request (status %d)\n", Status);
        goto Cleanup;
    }
    KdStatus = KdPacketDecode(PacketInfo.Payload,
                              PacketInfo.PayloadLength,
                              &KdPacketView);
    if (KdStatus != KdPacketStatusSuccess ||
        KdPacketView.Leader != KD_PACKET_LEADER_DATA ||
        KdPacketView.Type != KD_PACKET_TYPE_STATE_MANIPULATE ||
        KdPacketView.PayloadLength < sizeof(uint32_t) ||
        ReadLittleEndian32(KdPacketView.Payload) != DbgKdGetVersionApi)
    {
        fprintf(stderr, "expected DbgKdGetVersionApi (status %d)\n", KdStatus);
        goto Cleanup;
    }

    KdStatus = KdPacketEncodeControl(KD_PACKET_TYPE_ACKNOWLEDGE,
                                     KdPacketView.Id,
                                     KdPacket,
                                     sizeof(KdPacket),
                                     &KdPacketLength);
    if (KdStatus != KdPacketStatusSuccess)
    {
        fprintf(stderr, "could not encode request ACK (status %d)\n", KdStatus);
        goto Cleanup;
    }
    Status = KdNetEncodePacket(&CryptoContext,
                               1,
                               KDNET_PACKET_TYPE_DATA,
                               4,
                               KDNET_DIRECTION_TARGET,
                               KdPacket,
                               KdPacketLength,
                               Packet,
                               sizeof(Packet),
                               &PacketLength);
    if (Status != KdNetStatusSuccess ||
        !SendPacket(Socket, Packet, PacketLength))
    {
        fprintf(stderr, "could not send request ACK (status %d)\n", Status);
        goto Cleanup;
    }

    ZeroMemory(&VersionResponse, sizeof(VersionResponse));
    VersionResponse.ApiNumber = DbgKdGetVersionApi;
    VersionResponse.ProcessorLevel = 6;
    VersionResponse.Processor = 0;
    VersionResponse.ReturnStatus = STATUS_SUCCESS;
    VersionResponse.u.GetVersion64.MajorVersion = 6;
    VersionResponse.u.GetVersion64.MinorVersion = 7601;
    VersionResponse.u.GetVersion64.ProtocolVersion = DBGKD_64BIT_PROTOCOL_VERSION2;
    VersionResponse.u.GetVersion64.KdSecondaryVersion = CURRENT_KD_SECONDARY_VERSION;
    VersionResponse.u.GetVersion64.Flags = DBGKD_VERS_FLAG_DATA |
                                           DBGKD_VERS_FLAG_PTR64;
    VersionResponse.u.GetVersion64.MachineType = IMAGE_FILE_MACHINE_AMD64;
    VersionResponse.u.GetVersion64.MaxPacketType = KD_PACKET_TYPE_MAX;
    VersionResponse.u.GetVersion64.MaxStateChange =
        (UCHAR)DbgKdMaximumStateChange;
    VersionResponse.u.GetVersion64.MaxManipulate =
        (UCHAR)DbgKdMaximumManipulate;
    VersionResponse.u.GetVersion64.KernBase = 0xfffff80000000000ULL;
    VersionResponse.u.GetVersion64.PsLoadedModuleList = 0xfffff80000000800ULL;
    VersionResponse.u.GetVersion64.DebuggerDataList = 0xfffff80000001000ULL;

    KdStatus = KdPacketEncodeData(KD_PACKET_TYPE_STATE_MANIPULATE,
                                  KD_PACKET_INITIAL_ID ^ 1,
                                  (const unsigned char *)&VersionResponse,
                                  sizeof(VersionResponse),
                                  NULL,
                                  0,
                                  KdPacket,
                                  sizeof(KdPacket),
                                  &KdPacketLength);
    if (KdStatus != KdPacketStatusSuccess)
    {
        fprintf(stderr, "could not encode version response (status %d)\n", KdStatus);
        goto Cleanup;
    }
    Status = KdNetEncodePacket(&CryptoContext,
                               1,
                               KDNET_PACKET_TYPE_DATA,
                               5,
                               KDNET_DIRECTION_TARGET,
                               KdPacket,
                               KdPacketLength,
                               Packet,
                               sizeof(Packet),
                               &PacketLength);
    if (Status != KdNetStatusSuccess ||
        !SendPacket(Socket, Packet, PacketLength))
    {
        fprintf(stderr, "could not send version response (status %d)\n", Status);
        goto Cleanup;
    }

    if (!ReceivePacket(Socket, Packet, sizeof(Packet), &PacketLength))
        goto Cleanup;
    Status = KdNetDecodePacket(&CryptoContext,
                               Packet,
                               PacketLength,
                               &PacketInfo);
    if (Status != KdNetStatusSuccess)
    {
        fprintf(stderr, "could not decode version-response ACK (status %d)\n", Status);
        goto Cleanup;
    }
    KdStatus = KdPacketDecode(PacketInfo.Payload,
                              PacketInfo.PayloadLength,
                              &KdPacketView);
    if (KdStatus == KdPacketStatusSuccess &&
        KdPacketView.Leader == KD_PACKET_LEADER_CONTROL &&
        KdPacketView.Type == KD_PACKET_TYPE_ACKNOWLEDGE &&
        KdPacketView.Id == KD_PACKET_INITIAL_ID)
    {
        /* Rizin acknowledges the ACK sent for its GetVersion request. */
        if (!ReceivePacket(Socket, Packet, sizeof(Packet), &PacketLength))
            goto Cleanup;
        Status = KdNetDecodePacket(&CryptoContext,
                                   Packet,
                                   PacketLength,
                                   &PacketInfo);
        if (Status != KdNetStatusSuccess ||
            PacketInfo.Type != KDNET_PACKET_TYPE_DATA ||
            PacketInfo.Direction != KDNET_DIRECTION_DEBUGGER)
        {
            fprintf(stderr, "could not decode version-response ACK (status %d)\n",
                    Status);
            goto Cleanup;
        }
        KdStatus = KdPacketDecode(PacketInfo.Payload,
                                  PacketInfo.PayloadLength,
                                  &KdPacketView);
    }
    if (KdStatus != KdPacketStatusSuccess ||
        KdPacketView.Leader != KD_PACKET_LEADER_CONTROL ||
        KdPacketView.Type != KD_PACKET_TYPE_ACKNOWLEDGE ||
        KdPacketView.Id != (KD_PACKET_INITIAL_ID ^ 1))
    {
        fprintf(stderr, "invalid version-response ACK (status %d)\n", KdStatus);
        goto Cleanup;
    }

ReceiveManipulateRequest:
    if (!ReceivePacket(Socket, Packet, sizeof(Packet), &PacketLength))
        goto Cleanup;
    Status = KdNetDecodePacket(&CryptoContext,
                               Packet,
                               PacketLength,
                               &PacketInfo);
    if (Status != KdNetStatusSuccess ||
        PacketInfo.Type != KDNET_PACKET_TYPE_DATA ||
        PacketInfo.Direction != KDNET_DIRECTION_DEBUGGER)
    {
        fprintf(stderr, "could not decode debugger KD request (status %d)\n",
                Status);
        goto Cleanup;
    }
    KdStatus = KdPacketDecode(PacketInfo.Payload,
                              PacketInfo.PayloadLength,
                              &KdPacketView);
    if (KdStatus == KdPacketStatusSuccess &&
        KdPacketView.Leader == KD_PACKET_LEADER_CONTROL)
    {
        /* Rizin acknowledges control ACK packets; they carry no new request. */
        goto ReceiveManipulateRequest;
    }
    if (KdStatus != KdPacketStatusSuccess ||
        KdPacketView.Leader != KD_PACKET_LEADER_DATA ||
        KdPacketView.Type != KD_PACKET_TYPE_STATE_MANIPULATE ||
        KdPacketView.PayloadLength < sizeof(ManipulateRequest))
    {
        fprintf(stderr, "expected a KD manipulate request (status %d)\n", KdStatus);
        goto Cleanup;
    }

    CopyMemory(&ManipulateRequest,
               KdPacketView.Payload,
               sizeof(ManipulateRequest));
    RequestData = KdPacketView.Payload + sizeof(ManipulateRequest);
    RequestDataLength = KdPacketView.PayloadLength - sizeof(ManipulateRequest);

    /* Exercise the consumer's resend path once before accepting a write. */
    if (FullMode && !ResendInjected &&
        ManipulateRequest.ApiNumber == DbgKdWriteVirtualMemoryApi)
    {
        KdStatus = KdPacketEncodeControl(KD_PACKET_TYPE_RESEND,
                                         0,
                                         KdPacket,
                                         sizeof(KdPacket),
                                         &KdPacketLength);
        if (KdStatus != KdPacketStatusSuccess)
            goto Cleanup;
        Status = KdNetEncodePacket(&CryptoContext,
                                   1,
                                   KDNET_PACKET_TYPE_DATA,
                                   TargetSequence++,
                                   KDNET_DIRECTION_TARGET,
                                   KdPacket,
                                   KdPacketLength,
                                   Packet,
                                   sizeof(Packet),
                                   &PacketLength);
        if (Status != KdNetStatusSuccess ||
            !SendPacket(Socket, Packet, PacketLength))
        {
            goto Cleanup;
        }
        ResendInjected = 1;
        printf("RESEND injected; waiting for the repeated write request.\n");
        goto ReceiveManipulateRequest;
    }

    KdStatus = KdPacketEncodeControl(KD_PACKET_TYPE_ACKNOWLEDGE,
                                     KdPacketView.Id,
                                     KdPacket,
                                     sizeof(KdPacket),
                                     &KdPacketLength);
    if (KdStatus != KdPacketStatusSuccess)
    {
        fprintf(stderr, "could not encode request ACK (status %d)\n", KdStatus);
        goto Cleanup;
    }
    Status = KdNetEncodePacket(&CryptoContext,
                               1,
                               KDNET_PACKET_TYPE_DATA,
                               TargetSequence++,
                               KDNET_DIRECTION_TARGET,
                               KdPacket,
                               KdPacketLength,
                               Packet,
                               sizeof(Packet),
                               &PacketLength);
    if (Status != KdNetStatusSuccess ||
        !SendPacket(Socket, Packet, PacketLength))
    {
        fprintf(stderr, "could not send request ACK (status %d)\n", Status);
        goto Cleanup;
    }

    if (!FullMode)
    {
        if (ManipulateRequest.ApiNumber != DbgKdReadVirtualMemoryApi)
        {
            fprintf(stderr, "expected DbgKdReadVirtualMemoryApi\n");
            goto Cleanup;
        }
        printf("Inner KD synchronized; GetVersion replied; ReadVirtualMemory received.\n");
        Result = 0;
        goto Cleanup;
    }

    if (ManipulateRequest.ApiNumber == DbgKdContinueApi ||
        ManipulateRequest.ApiNumber == DbgKdContinueApi2)
    {
        printf("Continue received; full fake-target session completed.\n");
        Result = 0;
        goto Cleanup;
    }

    ManipulateResponse = ManipulateRequest;
    ManipulateResponse.ReturnStatus = STATUS_SUCCESS;
    ResponseData = NULL;
    ResponseDataLength = 0;

    switch (ManipulateRequest.ApiNumber)
    {
        case DbgKdReadVirtualMemoryApi:
            if (ManipulateRequest.u.ReadMemory.TargetBaseAddress <
                    SIMULATOR_MEMORY_BASE ||
                ManipulateRequest.u.ReadMemory.TargetBaseAddress -
                    SIMULATOR_MEMORY_BASE >= SIMULATOR_MEMORY_SIZE)
            {
                ManipulateResponse.ReturnStatus = STATUS_ACCESS_VIOLATION;
                ManipulateResponse.u.ReadMemory.ActualBytesRead = 0;
                break;
            }
            MemoryOffset = (size_t)
                (ManipulateRequest.u.ReadMemory.TargetBaseAddress -
                 SIMULATOR_MEMORY_BASE);
            ResponseDataLength = ManipulateRequest.u.ReadMemory.TransferCount;
            if (ResponseDataLength > SIMULATOR_MEMORY_SIZE - MemoryOffset)
                ResponseDataLength = SIMULATOR_MEMORY_SIZE - MemoryOffset;
            if (ResponseDataLength >
                KD_PACKET_MAX_PAYLOAD - sizeof(ManipulateResponse))
            {
                ResponseDataLength =
                    KD_PACKET_MAX_PAYLOAD - sizeof(ManipulateResponse);
            }
            ManipulateResponse.u.ReadMemory.ActualBytesRead =
                (ULONG)ResponseDataLength;
            ResponseData = TargetMemory + MemoryOffset;
            break;

        case DbgKdWriteVirtualMemoryApi:
            if (ManipulateRequest.u.WriteMemory.TargetBaseAddress <
                    SIMULATOR_MEMORY_BASE ||
                ManipulateRequest.u.WriteMemory.TargetBaseAddress -
                    SIMULATOR_MEMORY_BASE >= SIMULATOR_MEMORY_SIZE)
            {
                ManipulateResponse.ReturnStatus = STATUS_ACCESS_VIOLATION;
                ManipulateResponse.u.WriteMemory.ActualBytesWritten = 0;
                break;
            }
            MemoryOffset = (size_t)
                (ManipulateRequest.u.WriteMemory.TargetBaseAddress -
                 SIMULATOR_MEMORY_BASE);
            ResponseDataLength = ManipulateRequest.u.WriteMemory.TransferCount;
            if (ResponseDataLength > RequestDataLength ||
                ResponseDataLength > SIMULATOR_MEMORY_SIZE - MemoryOffset)
            {
                ManipulateResponse.ReturnStatus = STATUS_INVALID_PARAMETER;
                ManipulateResponse.u.WriteMemory.ActualBytesWritten = 0;
                ResponseDataLength = 0;
                break;
            }
            CopyMemory(TargetMemory + MemoryOffset,
                       RequestData,
                       ResponseDataLength);
            ManipulateResponse.u.WriteMemory.ActualBytesWritten =
                (ULONG)ResponseDataLength;
            ResponseDataLength = 0;
            printf("WriteVirtualMemory: %lu bytes.\n",
                   ManipulateResponse.u.WriteMemory.ActualBytesWritten);
            break;

        case DbgKdGetContextApi:
            ResponseData = ContextData;
            ResponseDataLength = sizeof(ContextData);
            printf("GetContext: %Iu bytes.\n", ResponseDataLength);
            break;

        case DbgKdSetContextApi:
            if (RequestDataLength > sizeof(ContextData))
            {
                ManipulateResponse.ReturnStatus = STATUS_INVALID_PARAMETER;
                break;
            }
            CopyMemory(ContextData, RequestData, RequestDataLength);
            printf("SetContext: %Iu bytes.\n", RequestDataLength);
            break;

        case DbgKdWriteBreakPointApi:
            BreakpointHandle = 0;
            for (Index = 0; Index < SIMULATOR_BREAKPOINTS; ++Index)
            {
                if (BreakpointAddresses[Index] == 0)
                {
                    BreakpointAddresses[Index] =
                        ManipulateRequest.u.WriteBreakPoint.BreakPointAddress;
                    BreakpointHandle = (unsigned long)Index + 1;
                    break;
                }
            }
            if (BreakpointHandle == 0)
                ManipulateResponse.ReturnStatus = STATUS_NO_MEMORY;
            ManipulateResponse.u.WriteBreakPoint.BreakPointHandle =
                BreakpointHandle;
            printf("WriteBreakPoint: handle %lu.\n", BreakpointHandle);
            break;

        case DbgKdRestoreBreakPointApi:
            BreakpointHandle =
                ManipulateRequest.u.RestoreBreakPoint.BreakPointHandle;
            if (BreakpointHandle == 0 ||
                BreakpointHandle > SIMULATOR_BREAKPOINTS ||
                BreakpointAddresses[BreakpointHandle - 1] == 0)
            {
                ManipulateResponse.ReturnStatus = STATUS_INVALID_PARAMETER;
            }
            else
            {
                BreakpointAddresses[BreakpointHandle - 1] = 0;
            }
            printf("RestoreBreakPoint: handle %lu.\n", BreakpointHandle);
            break;

        default:
            ManipulateResponse.ReturnStatus = STATUS_NOT_IMPLEMENTED;
            printf("Unsupported manipulate API %08lx.\n",
                   ManipulateRequest.ApiNumber);
            break;
    }

    KdStatus = KdPacketEncodeData(KD_PACKET_TYPE_STATE_MANIPULATE,
                                  TargetPacketId,
                                  (const unsigned char *)&ManipulateResponse,
                                  sizeof(ManipulateResponse),
                                  ResponseData,
                                  ResponseDataLength,
                                  LastKdPacket,
                                  sizeof(LastKdPacket),
                                  &LastKdPacketLength);
    if (KdStatus != KdPacketStatusSuccess)
    {
        fprintf(stderr, "could not encode manipulate response (status %d)\n",
                KdStatus);
        goto Cleanup;
    }

    RetryCount = 0;
ResendManipulateResponse:
    Status = KdNetEncodePacket(&CryptoContext,
                               1,
                               KDNET_PACKET_TYPE_DATA,
                               TargetSequence++,
                               KDNET_DIRECTION_TARGET,
                               LastKdPacket,
                               LastKdPacketLength,
                               Packet,
                               sizeof(Packet),
                               &PacketLength);
    if (Status != KdNetStatusSuccess ||
        !SendPacket(Socket, Packet, PacketLength))
    {
        fprintf(stderr, "could not send manipulate response (status %d)\n",
                Status);
        goto Cleanup;
    }

WaitForManipulateAcknowledge:
    if (!ReceivePacket(Socket, Packet, sizeof(Packet), &PacketLength))
        goto Cleanup;
    Status = KdNetDecodePacket(&CryptoContext,
                               Packet,
                               PacketLength,
                               &PacketInfo);
    if (Status != KdNetStatusSuccess)
    {
        fprintf(stderr, "could not decode response control (status %d)\n", Status);
        goto Cleanup;
    }
    KdStatus = KdPacketDecode(PacketInfo.Payload,
                              PacketInfo.PayloadLength,
                              &KdPacketView);
    if (KdStatus != KdPacketStatusSuccess ||
        KdPacketView.Leader != KD_PACKET_LEADER_CONTROL)
    {
        fprintf(stderr, "expected response control (status %d)\n", KdStatus);
        goto Cleanup;
    }
    if (KdPacketView.Type == KD_PACKET_TYPE_RESEND)
    {
        if (++RetryCount > 3)
        {
            fprintf(stderr, "manipulate response retry limit reached\n");
            goto Cleanup;
        }
        goto ResendManipulateResponse;
    }
    if (KdPacketView.Type != KD_PACKET_TYPE_ACKNOWLEDGE ||
        KdPacketView.Id != TargetPacketId)
    {
        /* Ignore an ACK-to-ACK and continue waiting for the response ACK. */
        goto WaitForManipulateAcknowledge;
    }

    TargetPacketId ^= 1;
    goto ReceiveManipulateRequest;

Cleanup:
    if (Socket != INVALID_SOCKET)
        closesocket(Socket);
    WSACleanup();
    return Result;
}
