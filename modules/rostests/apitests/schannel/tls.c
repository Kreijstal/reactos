/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Schannel client connection helpers shared by the tests
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

#include "schannel_apitest.h"

static BOOL recv_more(PTLS_CONNECTION Connection)
{
    int ret;

    if (Connection->RawLength >= TLS_RAW_SIZE)
    {
        ok(0, "Receive buffer full (%lu bytes)\n", Connection->RawLength);
        return FALSE;
    }
    ret = recv(Connection->Socket, (char *)Connection->Raw + Connection->RawLength,
               TLS_RAW_SIZE - Connection->RawLength, 0);
    if (ret <= 0)
    {
        ok(0, "recv returned %d, error %d\n", ret, WSAGetLastError());
        return FALSE;
    }
    Connection->RawLength += ret;
    return TRUE;
}

static BOOL send_all(SOCKET Socket, const void *Data, ULONG Length)
{
    const char *p = Data;
    int ret;

    while (Length)
    {
        ret = send(Socket, p, Length, 0);
        if (ret <= 0)
        {
            ok(0, "send returned %d, error %d\n", ret, WSAGetLastError());
            return FALSE;
        }
        p += ret;
        Length -= ret;
    }
    return TRUE;
}

static BOOL open_socket(PTLS_CONNECTION Connection)
{
    char server[300], *port;
    struct addrinfo hints, *res;
    WSADATA wsa_data;
    DWORD len;

    len = GetEnvironmentVariableA("SCHANNEL_APITEST_SERVER", server, sizeof(server));
    if (!len || len >= sizeof(server))
        strcpy(server, "test.winehq.org:443");
    port = strrchr(server, ':');
    if (!port)
    {
        skip("Bad SCHANNEL_APITEST_SERVER '%s'\n", server);
        return FALSE;
    }
    *port++ = 0;
    strcpy(Connection->Host, server);

    if (WSAStartup(MAKEWORD(2, 2), &wsa_data))
    {
        skip("WSAStartup failed\n");
        return FALSE;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(server, port, &hints, &res))
    {
        skip("Cannot resolve %s\n", server);
        return FALSE;
    }

    Connection->Socket = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (Connection->Socket == INVALID_SOCKET)
    {
        freeaddrinfo(res);
        skip("socket failed: %d\n", WSAGetLastError());
        return FALSE;
    }
    if (connect(Connection->Socket, res->ai_addr, (int)res->ai_addrlen))
    {
        freeaddrinfo(res);
        skip("Cannot connect to %s:%s: %d\n", server, port, WSAGetLastError());
        closesocket(Connection->Socket);
        Connection->Socket = INVALID_SOCKET;
        return FALSE;
    }
    freeaddrinfo(res);
    return TRUE;
}

BOOL TlsConnect(PTLS_CONNECTION Connection, DWORD Protocols)
{
    return TlsConnectAlpn(Connection, Protocols, NULL, 0);
}

BOOL TlsConnectAlpn(PTLS_CONNECTION Connection, DWORD Protocols, const BYTE *Alpn, USHORT AlpnSize)
{
    static const ULONG req = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
                             ISC_REQ_CONFIDENTIALITY | ISC_REQ_STREAM | ISC_REQ_MANUAL_CRED_VALIDATION;
    SCHANNEL_CRED cred;
    SecBuffer in_buffers[2], out_buffer;
    SecBufferDesc in_desc, out_desc;
    BYTE token[16384];
    BYTE alpn_buffer[FIELD_OFFSET(SEC_APPLICATION_PROTOCOLS, ProtocolLists[0].ProtocolList) + 256];
    SEC_APPLICATION_PROTOCOLS *alpn = (SEC_APPLICATION_PROTOCOLS *)alpn_buffer;
    SecBuffer alpn_in;
    SecBufferDesc alpn_desc;
    SECURITY_STATUS status;
    ULONG attrs;
    BOOL first = TRUE;

    memset(Connection, 0, sizeof(*Connection));
    Connection->Socket = INVALID_SOCKET;
    SecInvalidateHandle(&Connection->Credentials);
    SecInvalidateHandle(&Connection->Context);

    if (!open_socket(Connection))
        return FALSE;

    Connection->Raw = HeapAlloc(GetProcessHeap(), 0, TLS_RAW_SIZE);
    if (!Connection->Raw)
    {
        skip("Out of memory\n");
        TlsClose(Connection);
        return FALSE;
    }

    memset(&cred, 0, sizeof(cred));
    cred.dwVersion = SCHANNEL_CRED_VERSION;
    cred.grbitEnabledProtocols = Protocols;
    cred.dwFlags = SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_MANUAL_CRED_VALIDATION;
    status = AcquireCredentialsHandleW(NULL, (SEC_WCHAR *)UNISP_NAME_W, SECPKG_CRED_OUTBOUND, NULL,
                                       &cred, NULL, NULL, &Connection->Credentials, NULL);
    ok(status == SEC_E_OK, "AcquireCredentialsHandleW failed: %08lx\n", status);
    if (status != SEC_E_OK)
    {
        TlsClose(Connection);
        return FALSE;
    }

    for (;;)
    {
        out_buffer.BufferType = SECBUFFER_TOKEN;
        out_buffer.cbBuffer = sizeof(token);
        out_buffer.pvBuffer = token;
        out_desc.ulVersion = SECBUFFER_VERSION;
        out_desc.cBuffers = 1;
        out_desc.pBuffers = &out_buffer;

        in_buffers[0].BufferType = SECBUFFER_TOKEN;
        in_buffers[0].cbBuffer = Connection->RawLength;
        in_buffers[0].pvBuffer = Connection->Raw;
        in_buffers[1].BufferType = SECBUFFER_EMPTY;
        in_buffers[1].cbBuffer = 0;
        in_buffers[1].pvBuffer = NULL;
        in_desc.ulVersion = SECBUFFER_VERSION;
        in_desc.cBuffers = 2;
        in_desc.pBuffers = in_buffers;

        if (first && Alpn)
        {
            alpn->ProtocolListsSize = FIELD_OFFSET(SEC_APPLICATION_PROTOCOL_LIST, ProtocolList) + AlpnSize;
            alpn->ProtocolLists[0].ProtoNegoExt = SecApplicationProtocolNegotiationExt_ALPN;
            alpn->ProtocolLists[0].ProtocolListSize = AlpnSize;
            memcpy(alpn->ProtocolLists[0].ProtocolList, Alpn, AlpnSize);
            alpn_in.BufferType = SECBUFFER_APPLICATION_PROTOCOLS;
            alpn_in.cbBuffer = FIELD_OFFSET(SEC_APPLICATION_PROTOCOLS, ProtocolLists) + alpn->ProtocolListsSize;
            alpn_in.pvBuffer = alpn;
            alpn_desc.ulVersion = SECBUFFER_VERSION;
            alpn_desc.cBuffers = 1;
            alpn_desc.pBuffers = &alpn_in;
        }

        status = InitializeSecurityContextA(&Connection->Credentials, first ? NULL : &Connection->Context,
                                            Connection->Host, req, 0, 0,
                                            first ? (Alpn ? &alpn_desc : NULL) : &in_desc, 0,
                                            first ? &Connection->Context : NULL, &out_desc, &attrs, NULL);
        first = FALSE;

        if (status == SEC_E_INCOMPLETE_MESSAGE)
        {
            if (!recv_more(Connection))
                break;
            continue;
        }

        if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED)
        {
            ok(0, "InitializeSecurityContext failed: %08lx\n", status);
            break;
        }

        if (out_buffer.cbBuffer && !send_all(Connection->Socket, token, out_buffer.cbBuffer))
            break;

        /* Keep what the handshake did not consume */
        if (in_buffers[1].BufferType == SECBUFFER_EXTRA && in_buffers[1].cbBuffer)
        {
            memmove(Connection->Raw, Connection->Raw + Connection->RawLength - in_buffers[1].cbBuffer,
                    in_buffers[1].cbBuffer);
            Connection->RawLength = in_buffers[1].cbBuffer;
        }
        else
        {
            Connection->RawLength = 0;
        }

        if (status == SEC_E_OK)
            return TRUE;

        if (!Connection->RawLength && !recv_more(Connection))
            break;
    }

    TlsClose(Connection);
    return FALSE;
}

BOOL TlsSend(PTLS_CONNECTION Connection, const void *Data, ULONG Length)
{
    SecPkgContext_StreamSizes sizes;
    SecBuffer buffers[4];
    SecBufferDesc desc;
    SECURITY_STATUS status;
    BYTE *record;
    BOOL ret;

    status = QueryContextAttributesW(&Connection->Context, SECPKG_ATTR_STREAM_SIZES, &sizes);
    ok(status == SEC_E_OK, "STREAM_SIZES: got %08lx\n", status);
    if (status != SEC_E_OK || Length > sizes.cbMaximumMessage)
        return FALSE;

    record = HeapAlloc(GetProcessHeap(), 0, sizes.cbHeader + Length + sizes.cbTrailer);
    if (!record)
        return FALSE;
    memcpy(record + sizes.cbHeader, Data, Length);

    buffers[0].BufferType = SECBUFFER_STREAM_HEADER;
    buffers[0].cbBuffer = sizes.cbHeader;
    buffers[0].pvBuffer = record;
    buffers[1].BufferType = SECBUFFER_DATA;
    buffers[1].cbBuffer = Length;
    buffers[1].pvBuffer = record + sizes.cbHeader;
    buffers[2].BufferType = SECBUFFER_STREAM_TRAILER;
    buffers[2].cbBuffer = sizes.cbTrailer;
    buffers[2].pvBuffer = record + sizes.cbHeader + Length;
    buffers[3].BufferType = SECBUFFER_EMPTY;
    buffers[3].cbBuffer = 0;
    buffers[3].pvBuffer = NULL;
    desc.ulVersion = SECBUFFER_VERSION;
    desc.cBuffers = 4;
    desc.pBuffers = buffers;

    status = EncryptMessage(&Connection->Context, 0, &desc, 0);
    ok(status == SEC_E_OK, "EncryptMessage failed: %08lx\n", status);
    ret = (status == SEC_E_OK) &&
          send_all(Connection->Socket, record, buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer);
    HeapFree(GetProcessHeap(), 0, record);
    return ret;
}

void TlsClose(PTLS_CONNECTION Connection)
{
    if (SecIsValidHandle(&Connection->Context))
        DeleteSecurityContext(&Connection->Context);
    if (SecIsValidHandle(&Connection->Credentials))
        FreeCredentialsHandle(&Connection->Credentials);
    if (Connection->Socket != INVALID_SOCKET)
        closesocket(Connection->Socket);
    if (Connection->Raw)
        HeapFree(GetProcessHeap(), 0, Connection->Raw);
    memset(Connection, 0, sizeof(*Connection));
    Connection->Socket = INVALID_SOCKET;
}
