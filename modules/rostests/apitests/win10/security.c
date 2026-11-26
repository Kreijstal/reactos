/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Integrity ACE encoding and Windows-compatible package identity
 */
#include <apitest.h>

START_TEST(security)
{
    BOOL (WINAPI *AddLabel)(PACL, DWORD, DWORD, DWORD, PSID);
    HRESULT (WINAPI *Derive)(PCWSTR, PSID *);
    HMODULE Advapi = GetModuleHandleW(L"advapi32.dll"), Userenv = LoadLibraryW(L"userenv.dll");
    SID_IDENTIFIER_AUTHORITY Authority = SECURITY_MANDATORY_LABEL_AUTHORITY;
    union { ACL Acl; BYTE Bytes[128]; } Buffer;
    PSID Label = NULL, A = NULL, B = NULL;
    PSYSTEM_MANDATORY_LABEL_ACE Ace = NULL;
    DWORD Expected[] = {2,2912141003u,1046989974u,3114571674u,466992697u,
                        2296800646u,1885394386u,3702379317u};
    HRESULT Hr;
    unsigned i;

    AddLabel = (void *)GetProcAddress(Advapi, "AddMandatoryAce");
    if (AddLabel)
    {
        ok(AllocateAndInitializeSid(&Authority, 1, SECURITY_MANDATORY_MEDIUM_RID,
                                    0,0,0,0,0,0,0,&Label), "SID allocation failed\n");
        if (Label)
        {
            ok(InitializeAcl(&Buffer.Acl, sizeof(Buffer), ACL_REVISION), "ACL initialization failed\n");
            ok(AddLabel(&Buffer.Acl, ACL_REVISION, 0, SYSTEM_MANDATORY_LABEL_NO_WRITE_UP, Label),
               "Adding mandatory ACE failed: %lu\n", GetLastError());
            ok(IsValidAcl(&Buffer.Acl), "Resulting ACL is invalid\n");
            ok(Buffer.Acl.AceCount == 1, "Wrong ACE count\n");
            ok(GetAce(&Buffer.Acl, 0, (void **)&Ace), "GetAce failed\n");
            if (Ace)
            {
                ok(Ace->Header.AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE &&
                   Ace->Mask == SYSTEM_MANDATORY_LABEL_NO_WRITE_UP, "Wrong label header or mask\n");
                ok(EqualSid(&Ace->SidStart, Label), "Wrong integrity SID\n");
            }
            ok(!AddLabel(&Buffer.Acl, ACL_REVISION, 0, 0x80000000, Label), "Invalid mask accepted\n");
            ok(Buffer.Acl.AceCount == 1, "Rejected ACE changed ACL\n");
            FreeSid(Label);
        }
    }
    else win_skip("AddMandatoryAce unavailable\n");
    Derive = Userenv ? (void *)GetProcAddress(Userenv, "DeriveAppContainerSidFromAppContainerName") : NULL;
    if (!Derive) { win_skip("AppContainer SID API unavailable\n"); goto Done; }
    Hr = Derive(L"ReactOS.ApiTest", &A);
    ok_hex(Hr, S_OK);
    if (SUCCEEDED(Hr))
    {
        ok(IsValidSid(A) && *GetSidSubAuthorityCount(A) == ARRAYSIZE(Expected), "Invalid package SID\n");
        if (IsValidSid(A) && *GetSidSubAuthorityCount(A) == ARRAYSIZE(Expected))
            for (i = 0; i < ARRAYSIZE(Expected); ++i)
                ok(*GetSidSubAuthority(A, i) == Expected[i], "Wrong package RID %u\n", i);
    }
    Hr = Derive(L"REACTOS.APITEST", &B);
    ok_hex(Hr, S_OK);
    if (A && B) ok(EqualSid(A, B), "Name comparison was not case insensitive\n");
    if (B) { FreeSid(B); B = NULL; }
    ok_hex(Derive(L"", &B), E_INVALIDARG);
    ok(B == NULL, "Failure returned a SID\n");
Done:
    if (A) FreeSid(A);
    if (B) FreeSid(B);
    if (Userenv) FreeLibrary(Userenv);
}
