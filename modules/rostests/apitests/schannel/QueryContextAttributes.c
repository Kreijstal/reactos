/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Tests for the Schannel context attributes of a completed TLS handshake
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

#include "schannel_apitest.h"

/* What Windows 10 reports for the TLS 1.2 suites a client negotiates in practice.
 * AEAD suites have no separate MAC, so szHash is empty and dwHashLen 0. */
static const struct
{
    DWORD Suite;
    PCWSTR Name;
    PCWSTR Cipher;
    DWORD CipherLen;
    PCWSTR Hash;
    DWORD HashLen;
    PCWSTR Exchange;
    PCWSTR Certificate;
} KnownSuites[] =
{
    { 0xc02b, L"TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256", L"AES", 128, L"", 0, L"ECDH", L"ECDSA" },
    { 0xc02c, L"TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384", L"AES", 256, L"", 0, L"ECDH", L"ECDSA" },
    { 0xc02f, L"TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256", L"AES", 128, L"", 0, L"ECDH", L"RSA" },
    { 0xc030, L"TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384", L"AES", 256, L"", 0, L"ECDH", L"RSA" },
    { 0xc023, L"TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256", L"AES", 128, L"SHA256", 256, L"ECDH", L"ECDSA" },
    { 0xc024, L"TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA384", L"AES", 256, L"SHA384", 384, L"ECDH", L"ECDSA" },
    { 0xc027, L"TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256", L"AES", 128, L"SHA256", 256, L"ECDH", L"RSA" },
    { 0xc028, L"TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA384", L"AES", 256, L"SHA384", 384, L"ECDH", L"RSA" },
    { 0xc009, L"TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA", L"AES", 128, L"SHA1", 160, L"ECDH", L"ECDSA" },
    { 0xc00a, L"TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA", L"AES", 256, L"SHA1", 160, L"ECDH", L"ECDSA" },
    { 0xc013, L"TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA", L"AES", 128, L"SHA1", 160, L"ECDH", L"RSA" },
    { 0xc014, L"TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA", L"AES", 256, L"SHA1", 160, L"ECDH", L"RSA" },
    { 0x009e, L"TLS_DHE_RSA_WITH_AES_128_GCM_SHA256", L"AES", 128, L"", 0, L"DH", L"RSA" },
    { 0x009f, L"TLS_DHE_RSA_WITH_AES_256_GCM_SHA384", L"AES", 256, L"", 0, L"DH", L"RSA" },
    { 0x009c, L"TLS_RSA_WITH_AES_128_GCM_SHA256", L"AES", 128, L"", 0, L"RSA", L"RSA" },
    { 0x009d, L"TLS_RSA_WITH_AES_256_GCM_SHA384", L"AES", 256, L"", 0, L"RSA", L"RSA" },
    { 0x002f, L"TLS_RSA_WITH_AES_128_CBC_SHA", L"AES", 128, L"SHA1", 160, L"RSA", L"RSA" },
    { 0x0035, L"TLS_RSA_WITH_AES_256_CBC_SHA", L"AES", 256, L"SHA1", 160, L"RSA", L"RSA" },
};

static void test_cipher_info(CtxtHandle *context)
{
    SecPkgContext_CipherInfo info;
    SecPkgContext_ConnectionInfo conn_info;
    SECURITY_STATUS status;
    ULONG i;

    memset(&conn_info, 0, sizeof(conn_info));
    status = QueryContextAttributesW(context, SECPKG_ATTR_CONNECTION_INFO, &conn_info);
    ok(status == SEC_E_OK, "CONNECTION_INFO: got %08lx\n", status);
    ok(conn_info.dwProtocol == SP_PROT_TLS1_2_CLIENT, "CONNECTION_INFO: got protocol %#lx\n", conn_info.dwProtocol);

    /* What aria2 (WinTLSSession::getProtocolVersion) and .NET (SslStream.NegotiatedCipherSuite) ask for */
    memset(&info, 0xcc, sizeof(info));
    info.dwVersion = SECPKGCONTEXT_CIPHERINFO_V1;
    status = QueryContextAttributesW(context, SECPKG_ATTR_CIPHER_INFO, &info);
    ok(status == SEC_E_OK, "CIPHER_INFO: got %08lx\n", status);
    if (status != SEC_E_OK)
        return;

    ok(info.dwVersion == SECPKGCONTEXT_CIPHERINFO_V1, "got version %lu\n", info.dwVersion);
    /* The wire version, not an SP_PROT_* flag */
    ok(info.dwProtocol == 0x303, "got protocol %#lx\n", info.dwProtocol);
    ok(info.dwBaseCipherSuite == info.dwCipherSuite, "got base suite %#lx, suite %#lx\n",
       info.dwBaseCipherSuite, info.dwCipherSuite);
    ok(info.dwCipherLen == conn_info.dwCipherStrength, "got cipher length %lu, connection info says %lu\n",
       info.dwCipherLen, conn_info.dwCipherStrength);
    ok(info.dwCipherBlockLen == 16, "got cipher block length %lu\n", info.dwCipherBlockLen);
    ok(info.dwMinExchangeLen == 0, "got min exchange length %lu\n", info.dwMinExchangeLen);
    ok(info.dwMaxExchangeLen == 65536, "got max exchange length %lu\n", info.dwMaxExchangeLen);

    for (i = 0; i < _countof(KnownSuites); i++)
    {
        if (KnownSuites[i].Suite == info.dwCipherSuite)
            break;
    }
    ok(i < _countof(KnownSuites), "unexpected suite %#lx %S\n", info.dwCipherSuite, info.szCipherSuite);
    if (i == _countof(KnownSuites))
        return;

    trace("negotiated %S, key type %#lx\n", info.szCipherSuite, info.dwKeyType);
    ok(!wcscmp(info.szCipherSuite, KnownSuites[i].Name), "got suite name %S\n", info.szCipherSuite);
    ok(!wcscmp(info.szCipher, KnownSuites[i].Cipher), "got cipher %S\n", info.szCipher);
    ok(info.dwCipherLen == KnownSuites[i].CipherLen, "got cipher length %lu\n", info.dwCipherLen);
    ok(!wcscmp(info.szHash, KnownSuites[i].Hash), "got hash %S\n", info.szHash);
    ok(info.dwHashLen == KnownSuites[i].HashLen, "got hash length %lu\n", info.dwHashLen);
    ok(!wcscmp(info.szExchange, KnownSuites[i].Exchange), "got exchange %S\n", info.szExchange);
    ok(!wcscmp(info.szCertificate, KnownSuites[i].Certificate), "got certificate %S\n", info.szCertificate);
    /* For an (EC)DHE exchange over a named group, the TLS NamedGroup id */
    if (!wcscmp(KnownSuites[i].Exchange, L"ECDH"))
    {
        ok(info.dwKeyType == 0x17 || info.dwKeyType == 0x18 || info.dwKeyType == 0x19 || info.dwKeyType == 0x1d,
           "got key type %#lx\n", info.dwKeyType);
    }

    /* The ANSI entry point returns the same structure */
    memset(&info, 0xcc, sizeof(info));
    info.dwVersion = SECPKGCONTEXT_CIPHERINFO_V1;
    status = QueryContextAttributesA(context, SECPKG_ATTR_CIPHER_INFO, &info);
    ok(status == SEC_E_OK, "CIPHER_INFO (A): got %08lx\n", status);
    ok(info.dwProtocol == 0x303, "CIPHER_INFO (A): got protocol %#lx\n", info.dwProtocol);
    ok(!wcscmp(info.szCipherSuite, KnownSuites[i].Name), "CIPHER_INFO (A): got suite name %S\n", info.szCipherSuite);
}

static void test_application_protocol(BOOL offered)
{
    /* What .NET's HttpClient offers for an HTTP/1.1 request */
    static const BYTE http11[] = { 8, 'h','t','t','p','/','1','.','1' };
    SecPkgContext_ApplicationProtocol protocol;
    SECURITY_STATUS status;
    TLS_CONNECTION conn;

    if (!TlsConnectAlpn(&conn, SP_PROT_TLS1_2_CLIENT, offered ? http11 : NULL, sizeof(http11)))
        return;

    memset(&protocol, 0xcc, sizeof(protocol));
    status = QueryContextAttributesA(&conn.Context, SECPKG_ATTR_APPLICATION_PROTOCOL, &protocol);
    ok(status == SEC_E_OK, "APPLICATION_PROTOCOL (offered %d): got %08lx\n", offered, status);
    if (status == SEC_E_OK && !offered)
    {
        ok(protocol.ProtoNegoStatus == SecApplicationProtocolNegotiationStatus_None, "got status %d\n", protocol.ProtoNegoStatus);
        ok(protocol.ProtoNegoExt == SecApplicationProtocolNegotiationExt_None, "got extension %d\n", protocol.ProtoNegoExt);
        ok(protocol.ProtocolIdSize == 0, "got size %u\n", protocol.ProtocolIdSize);
    }
    else if (status == SEC_E_OK)
    {
        /* Servers that speak HTTP/1.1 answer with it, those that do not know ALPN do not answer */
        ok(protocol.ProtoNegoStatus == SecApplicationProtocolNegotiationStatus_Success ||
           protocol.ProtoNegoStatus == SecApplicationProtocolNegotiationStatus_None, "got status %d\n", protocol.ProtoNegoStatus);
        if (protocol.ProtoNegoStatus == SecApplicationProtocolNegotiationStatus_Success)
        {
            ok(protocol.ProtoNegoExt == SecApplicationProtocolNegotiationExt_ALPN, "got extension %d\n", protocol.ProtoNegoExt);
            ok(protocol.ProtocolIdSize == 8 && !memcmp(protocol.ProtocolId, "http/1.1", 8),
               "got protocol %.*s\n", protocol.ProtocolIdSize, protocol.ProtocolId);
        }
        trace("ALPN status %d\n", protocol.ProtoNegoStatus);
    }

    TlsClose(&conn);
}

START_TEST(QueryContextAttributes)
{
    TLS_CONNECTION conn;

    if (!TlsConnect(&conn, SP_PROT_TLS1_2_CLIENT))
        return;

    test_cipher_info(&conn.Context);

    TlsClose(&conn);

    test_application_protocol(FALSE);
    test_application_protocol(TRUE);
}
