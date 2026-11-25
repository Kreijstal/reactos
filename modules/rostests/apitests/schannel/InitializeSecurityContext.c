/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Tests for the context attributes returned by the Schannel
 *              InitializeSecurityContext
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

#include <apitest.h>
#include <windef.h>
#include <winbase.h>
#include <wincrypt.h>
#define SECURITY_WIN32
#include <security.h>
#include <schannel.h>

/* Schannel always provides sequence and replay detection, confidentiality
 * and a stream-oriented context; the remaining ISC_RET_* bits echo what was
 * requested. Applications such as aria2 (WinTLS) and curl fail the connection
 * when the returned attributes do not cover the requested ones. */
#define ALWAYS_RET (ISC_RET_REPLAY_DETECT | ISC_RET_SEQUENCE_DETECT | ISC_RET_CONFIDENTIALITY | ISC_RET_STREAM)

static void test_first_call(CredHandle *cred, ULONG req, ULONG expected)
{
    SecBuffer out_buffer = { 0, SECBUFFER_TOKEN, NULL };
    SecBufferDesc out_desc = { SECBUFFER_VERSION, 1, &out_buffer };
    BYTE token[16384];
    CtxtHandle context;
    SECURITY_STATUS status;
    ULONG attrs = 0xdeadbeef;

    if (!(req & ISC_REQ_ALLOCATE_MEMORY))
    {
        out_buffer.cbBuffer = sizeof(token);
        out_buffer.pvBuffer = token;
    }

    status = InitializeSecurityContextA(cred, NULL, (SEC_CHAR *)"localhost", req, 0, 0, NULL, 0,
                                        &context, &out_desc, &attrs, NULL);
    ok(status == SEC_I_CONTINUE_NEEDED, "req %08lx: got status %08lx\n", req, status);
    if (status != SEC_I_CONTINUE_NEEDED)
        return;

    ok(out_buffer.cbBuffer > 5 && out_buffer.pvBuffer && ((BYTE *)out_buffer.pvBuffer)[0] == 0x16,
       "req %08lx: no TLS handshake record in the output token\n", req);
    ok(attrs == expected, "req %08lx: got attributes %08lx, expected %08lx\n", req, attrs, expected);
    ok((attrs & req) == req, "req %08lx: requested attributes %08lx not returned\n", req, req & ~attrs);

    if (req & ISC_REQ_ALLOCATE_MEMORY)
        FreeContextBuffer(out_buffer.pvBuffer);
    DeleteSecurityContext(&context);
}

/* A server flight that stops after the ServerHello (TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256),
 * as when its Certificate record has not arrived yet. */
static const BYTE server_hello_only[] =
{
    0x16, 0x03, 0x03, 0x00, 0x2a,           /* handshake record, 42 bytes */
    0x02, 0x00, 0x00, 0x26,                 /* ServerHello, 38 bytes */
    0x03, 0x03,                             /* TLS 1.2 */
    0x5c, 0x1e, 0x8a, 0x47, 0x02, 0xd3, 0x6b, 0x91, 0x3f, 0xa0, 0x74, 0xe8, 0x15, 0xc9, 0x2d, 0x60,
    0xb7, 0x48, 0x0e, 0xf3, 0x9a, 0x21, 0x56, 0xcd, 0x83, 0x6f, 0x12, 0xe4, 0x3b, 0xa9, 0x70, 0x05,
    0x00,                                   /* no session ID */
    0xc0, 0x2f,                             /* cipher suite */
    0x00,                                   /* no compression */
};

/* When a handshake step leaves nothing to send, the output token must say so
 * (cbBuffer 0) instead of keeping the size of the caller's buffer, or the
 * caller sends that stale buffer to the server. */
static void test_nothing_to_send(ULONG req)
{
    SecBuffer out_buffer = { 0, SECBUFFER_TOKEN, NULL };
    SecBufferDesc out_desc = { SECBUFFER_VERSION, 1, &out_buffer };
    SecBuffer in_buffers[2];
    SecBufferDesc in_desc = { SECBUFFER_VERSION, 2, in_buffers };
    BYTE token[16384], input[sizeof(server_hello_only)];
    SCHANNEL_CRED schannel_cred;
    CredHandle cred_handle, *cred = &cred_handle;
    CtxtHandle context;
    SECURITY_STATUS status;
    ULONG attrs;

    memset(&schannel_cred, 0, sizeof(schannel_cred));
    schannel_cred.dwVersion = SCHANNEL_CRED_VERSION;
    schannel_cred.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
    schannel_cred.dwFlags = SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_MANUAL_CRED_VALIDATION;
    status = AcquireCredentialsHandleA(NULL, (SEC_CHAR *)UNISP_NAME_A, SECPKG_CRED_OUTBOUND, NULL,
                                       &schannel_cred, NULL, NULL, cred, NULL);
    ok(status == SEC_E_OK, "AcquireCredentialsHandleA failed: %08lx\n", status);
    if (status != SEC_E_OK)
        return;

    if (!(req & ISC_REQ_ALLOCATE_MEMORY))
    {
        out_buffer.cbBuffer = sizeof(token);
        out_buffer.pvBuffer = token;
    }
    status = InitializeSecurityContextA(cred, NULL, (SEC_CHAR *)"localhost", req, 0, 0, NULL, 0,
                                        &context, &out_desc, &attrs, NULL);
    ok(status == SEC_I_CONTINUE_NEEDED, "req %08lx: first call got status %08lx\n", req, status);
    if (status != SEC_I_CONTINUE_NEEDED)
    {
        FreeCredentialsHandle(cred);
        return;
    }
    if (req & ISC_REQ_ALLOCATE_MEMORY)
    {
        FreeContextBuffer(out_buffer.pvBuffer);
        out_buffer.cbBuffer = 0;
        out_buffer.pvBuffer = NULL;
    }
    else
    {
        out_buffer.cbBuffer = sizeof(token);
    }
    out_buffer.BufferType = SECBUFFER_TOKEN;

    memcpy(input, server_hello_only, sizeof(input));
    in_buffers[0].BufferType = SECBUFFER_TOKEN;
    in_buffers[0].cbBuffer = sizeof(input);
    in_buffers[0].pvBuffer = input;
    in_buffers[1].BufferType = SECBUFFER_EMPTY;
    in_buffers[1].cbBuffer = 0;
    in_buffers[1].pvBuffer = NULL;

    status = InitializeSecurityContextA(cred, &context, (SEC_CHAR *)"localhost", req, 0, 0, &in_desc, 0,
                                        NULL, &out_desc, &attrs, NULL);
    ok(status == SEC_I_CONTINUE_NEEDED || status == SEC_E_INCOMPLETE_MESSAGE,
       "req %08lx: ServerHello alone got status %08lx\n", req, status);
    if (status == SEC_I_CONTINUE_NEEDED)
    {
        ok(out_buffer.cbBuffer == 0, "req %08lx: got an output token of %lu bytes with nothing to send\n",
           req, out_buffer.cbBuffer);
        if (req & ISC_REQ_ALLOCATE_MEMORY)
            ok(out_buffer.pvBuffer == NULL, "req %08lx: got an allocated output token\n", req);
        ok(in_buffers[1].BufferType != SECBUFFER_EXTRA,
           "req %08lx: %lu bytes of a complete record left unconsumed\n", req, in_buffers[1].cbBuffer);
    }
    if ((req & ISC_REQ_ALLOCATE_MEMORY) && out_buffer.pvBuffer)
        FreeContextBuffer(out_buffer.pvBuffer);

    DeleteSecurityContext(&context);
    FreeCredentialsHandle(cred);
}

START_TEST(InitializeSecurityContext)
{
    SCHANNEL_CRED schannel_cred;
    CredHandle cred;
    SECURITY_STATUS status;

    memset(&schannel_cred, 0, sizeof(schannel_cred));
    schannel_cred.dwVersion = SCHANNEL_CRED_VERSION;
    schannel_cred.grbitEnabledProtocols = SP_PROT_TLS1_CLIENT;
    schannel_cred.dwFlags = SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_MANUAL_CRED_VALIDATION;

    status = AcquireCredentialsHandleA(NULL, (SEC_CHAR *)UNISP_NAME_A, SECPKG_CRED_OUTBOUND, NULL,
                                       &schannel_cred, NULL, NULL, &cred, NULL);
    ok(status == SEC_E_OK, "AcquireCredentialsHandleA failed: %08lx\n", status);
    if (status != SEC_E_OK)
        return;

    /* What aria2 WinTLS requests (0x819c) */
    test_first_call(&cred,
                    ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                    ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_USE_SUPPLIED_CREDS,
                    ALWAYS_RET | ISC_RET_ALLOCATED_MEMORY | ISC_RET_USED_SUPPLIED_CREDS);
    /* What curl requests */
    test_first_call(&cred,
                    ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                    ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_EXTENDED_ERROR |
                    ISC_REQ_MANUAL_CRED_VALIDATION,
                    ALWAYS_RET | ISC_RET_ALLOCATED_MEMORY | ISC_RET_EXTENDED_ERROR |
                    ISC_RET_MANUAL_CRED_VALIDATION);
    /* Caller-supplied output buffer, minimal request */
    test_first_call(&cred, ISC_REQ_CONFIDENTIALITY | ISC_REQ_STREAM, ALWAYS_RET);
    test_first_call(&cred, 0, ALWAYS_RET);

    FreeCredentialsHandle(&cred);

    test_nothing_to_send(ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                         ISC_REQ_STREAM | ISC_REQ_MANUAL_CRED_VALIDATION);
    test_nothing_to_send(ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                         ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_MANUAL_CRED_VALIDATION);
}
