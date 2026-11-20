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
}
