/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Schannel client connection helpers shared by the tests
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

#pragma once

#include <apitest.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windef.h>
#include <winbase.h>
#include <wincrypt.h>
#define SECURITY_WIN32
#include <security.h>
#include <schannel.h>

#define TLS_RAW_SIZE (64 * 1024)

typedef struct _TLS_CONNECTION
{
    SOCKET Socket;
    CredHandle Credentials;
    CtxtHandle Context;
    char Host[256];
    /* Received bytes that DecryptMessage has not consumed yet */
    BYTE *Raw;
    ULONG RawLength;
} TLS_CONNECTION, *PTLS_CONNECTION;

/* Connects to the server named by the SCHANNEL_APITEST_SERVER environment
 * variable ("host:port"), test.winehq.org:443 by default, and completes a
 * TLS handshake. Skips and returns FALSE when the server cannot be reached. */
BOOL TlsConnect(PTLS_CONNECTION Connection, DWORD Protocols);
/* The same, offering the ALPN protocols of a wire-format list (length byte + id, ...) */
BOOL TlsConnectAlpn(PTLS_CONNECTION Connection, DWORD Protocols, const BYTE *Alpn, USHORT AlpnSize);
BOOL TlsSend(PTLS_CONNECTION Connection, const void *Data, ULONG Length);
void TlsClose(PTLS_CONNECTION Connection);
