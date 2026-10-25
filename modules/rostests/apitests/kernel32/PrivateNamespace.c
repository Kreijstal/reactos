/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Tests for boundary descriptors and private namespaces
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */
#include "precomp.h"

typedef HANDLE (WINAPI *FN_CreateBoundaryDescriptorW)(LPCWSTR, ULONG);
typedef BOOL (WINAPI *FN_AddSIDToBoundaryDescriptor)(HANDLE *, PSID);
typedef VOID (WINAPI *FN_DeleteBoundaryDescriptor)(HANDLE);
typedef HANDLE (WINAPI *FN_CreatePrivateNamespaceW)(LPSECURITY_ATTRIBUTES, LPVOID, LPCWSTR);
typedef HANDLE (WINAPI *FN_OpenPrivateNamespaceW)(LPVOID, LPCWSTR);
typedef BOOLEAN (WINAPI *FN_ClosePrivateNamespace)(HANDLE, ULONG);

static FN_CreateBoundaryDescriptorW pCreateBoundaryDescriptorW;
static FN_AddSIDToBoundaryDescriptor pAddSIDToBoundaryDescriptor;
static FN_DeleteBoundaryDescriptor pDeleteBoundaryDescriptor;
static FN_CreatePrivateNamespaceW pCreatePrivateNamespaceW;
static FN_OpenPrivateNamespaceW pOpenPrivateNamespaceW;
static FN_ClosePrivateNamespace pClosePrivateNamespace;

#ifndef PRIVATE_NAMESPACE_FLAG_DESTROY
#define PRIVATE_NAMESPACE_FLAG_DESTROY 0x00000001
#endif

/* A boundary named after this process, with the user SID of our token */
static HANDLE
CreateTestBoundary(PCWSTR Suffix)
{
    WCHAR Name[64];
    HANDLE Boundary, Token;
    struct
    {
        TOKEN_USER User;
        BYTE Sid[SECURITY_MAX_SID_SIZE];
    } UserInfo;
    DWORD Size;

    swprintf(Name, _countof(Name), L"PrivateNamespaceApitest-%lu%s", GetCurrentProcessId(), Suffix);
    Boundary = pCreateBoundaryDescriptorW(Name, 0);
    ok(Boundary != NULL, "CreateBoundaryDescriptorW failed with %lu\n", GetLastError());
    if (!Boundary)
        return NULL;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token) ||
        !GetTokenInformation(Token, TokenUser, &UserInfo, sizeof(UserInfo), &Size))
    {
        skip("Cannot get the user SID: %lu\n", GetLastError());
        return Boundary;
    }
    CloseHandle(Token);

    ok(pAddSIDToBoundaryDescriptor(&Boundary, UserInfo.User.User.Sid),
       "AddSIDToBoundaryDescriptor failed with %lu\n", GetLastError());
    return Boundary;
}

START_TEST(PrivateNamespace)
{
    HMODULE Kernel32 = GetModuleHandleW(L"kernel32.dll");
    WCHAR MutexName[64], Qualified[96], Other[96];
    HANDLE Boundary, Foreign, Namespace, Namespace2, Mutex, Mutex2;
    SID_IDENTIFIER_AUTHORITY NtAuthority = {SECURITY_NT_AUTHORITY};
    PSID StrangerSid;
    DWORD Error;

    pCreateBoundaryDescriptorW = (FN_CreateBoundaryDescriptorW)GetProcAddress(Kernel32, "CreateBoundaryDescriptorW");
    pAddSIDToBoundaryDescriptor = (FN_AddSIDToBoundaryDescriptor)GetProcAddress(Kernel32, "AddSIDToBoundaryDescriptor");
    pDeleteBoundaryDescriptor = (FN_DeleteBoundaryDescriptor)GetProcAddress(Kernel32, "DeleteBoundaryDescriptor");
    pCreatePrivateNamespaceW = (FN_CreatePrivateNamespaceW)GetProcAddress(Kernel32, "CreatePrivateNamespaceW");
    pOpenPrivateNamespaceW = (FN_OpenPrivateNamespaceW)GetProcAddress(Kernel32, "OpenPrivateNamespaceW");
    pClosePrivateNamespace = (FN_ClosePrivateNamespace)GetProcAddress(Kernel32, "ClosePrivateNamespace");
    if (!pCreateBoundaryDescriptorW || !pAddSIDToBoundaryDescriptor || !pDeleteBoundaryDescriptor ||
        !pCreatePrivateNamespaceW || !pOpenPrivateNamespaceW || !pClosePrivateNamespace)
    {
        skip("Private namespaces are not available\n");
        return;
    }

    swprintf(MutexName, _countof(MutexName), L"PrivateNamespaceApitest-%lu-Mutex", GetCurrentProcessId());
    swprintf(Qualified, _countof(Qualified), L"ApitestNs\\%s", MutexName);
    swprintf(Other, _countof(Other), L"ApitestOther\\%s", MutexName);

    Boundary = CreateTestBoundary(L"");
    if (!Boundary)
        return;

    /* Create the namespace; the same boundary cannot be created twice */
    SetLastError(0xdeadbeef);
    Namespace = pCreatePrivateNamespaceW(NULL, Boundary, L"ApitestNs");
    ok(Namespace != NULL, "CreatePrivateNamespaceW failed with %lu\n", GetLastError());
    if (!Namespace)
        goto cleanup;

    SetLastError(0xdeadbeef);
    Namespace2 = pCreatePrivateNamespaceW(NULL, Boundary, L"ApitestNs2");
    Error = GetLastError();
    ok(Namespace2 == NULL, "Second CreatePrivateNamespaceW succeeded\n");
    ok(Error == ERROR_ALREADY_EXISTS, "Error = %lu\n", Error);

    /* Objects named Alias\Name live in the namespace, not in BaseNamedObjects */
    SetLastError(0xdeadbeef);
    Mutex = CreateMutexW(NULL, FALSE, Qualified);
    Error = GetLastError();
    ok(Mutex != NULL, "CreateMutexW failed with %lu\n", Error);
    ok(Error == ERROR_SUCCESS, "Error = %lu\n", Error);

    SetLastError(0xdeadbeef);
    Mutex2 = CreateMutexW(NULL, FALSE, Qualified);
    Error = GetLastError();
    ok(Mutex2 != NULL, "Second CreateMutexW failed with %lu\n", Error);
    ok(Error == ERROR_ALREADY_EXISTS, "Error = %lu\n", Error);
    if (Mutex2) CloseHandle(Mutex2);

    Mutex2 = OpenMutexW(SYNCHRONIZE, FALSE, Qualified);
    ok(Mutex2 != NULL, "OpenMutexW failed with %lu\n", GetLastError());
    if (Mutex2) CloseHandle(Mutex2);

    SetLastError(0xdeadbeef);
    Mutex2 = OpenMutexW(SYNCHRONIZE, FALSE, MutexName);
    Error = GetLastError();
    ok(Mutex2 == NULL, "The mutex is in BaseNamedObjects\n");
    ok(Error == ERROR_FILE_NOT_FOUND, "Error = %lu\n", Error);
    if (Mutex2) CloseHandle(Mutex2);

    /* Opening the boundary under another alias finds the same namespace */
    SetLastError(0xdeadbeef);
    Namespace2 = pOpenPrivateNamespaceW(Boundary, L"ApitestOther");
    ok(Namespace2 != NULL, "OpenPrivateNamespaceW failed with %lu\n", GetLastError());
    if (Namespace2)
    {
        SetLastError(0xdeadbeef);
        Mutex2 = CreateMutexW(NULL, FALSE, Other);
        Error = GetLastError();
        ok(Mutex2 != NULL, "CreateMutexW failed with %lu\n", Error);
        ok(Error == ERROR_ALREADY_EXISTS, "Error = %lu\n", Error);
        if (Mutex2) CloseHandle(Mutex2);
        ok(pClosePrivateNamespace(Namespace2, 0), "ClosePrivateNamespace failed with %lu\n", GetLastError());
    }

    /* After destroying it, the namespace cannot be opened, but a new one can be created */
    ok(pClosePrivateNamespace(Namespace, PRIVATE_NAMESPACE_FLAG_DESTROY),
       "ClosePrivateNamespace failed with %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    Namespace2 = pOpenPrivateNamespaceW(Boundary, L"ApitestNs");
    Error = GetLastError();
    ok(Namespace2 == NULL, "OpenPrivateNamespaceW opened a destroyed namespace\n");
    ok(Error == ERROR_PATH_NOT_FOUND, "Error = %lu\n", Error);
    if (Namespace2) pClosePrivateNamespace(Namespace2, 0);

    Namespace = pCreatePrivateNamespaceW(NULL, Boundary, L"ApitestNs");
    ok(Namespace != NULL, "CreatePrivateNamespaceW after destroying failed with %lu\n", GetLastError());
    if (Namespace)
    {
        /* A new, empty namespace */
        SetLastError(0xdeadbeef);
        Mutex2 = OpenMutexW(SYNCHRONIZE, FALSE, Qualified);
        Error = GetLastError();
        ok(Mutex2 == NULL, "The old mutex is in the new namespace\n");
        ok(Error == ERROR_FILE_NOT_FOUND, "Error = %lu\n", Error);
        if (Mutex2) CloseHandle(Mutex2);
        ok(pClosePrivateNamespace(Namespace, PRIVATE_NAMESPACE_FLAG_DESTROY),
           "ClosePrivateNamespace failed with %lu\n", GetLastError());
    }

    if (Mutex) CloseHandle(Mutex);

    /* Without the boundary's SIDs in the token the namespace cannot be created */
    Foreign = CreateTestBoundary(L"-Foreign");
    if (Foreign &&
        AllocateAndInitializeSid(&NtAuthority, 5, SECURITY_NT_NON_UNIQUE, 1, 2, 3, 4, 0, 0, 0, &StrangerSid))
    {
        ok(pAddSIDToBoundaryDescriptor(&Foreign, StrangerSid),
           "AddSIDToBoundaryDescriptor failed with %lu\n", GetLastError());
        SetLastError(0xdeadbeef);
        Namespace = pCreatePrivateNamespaceW(NULL, Foreign, L"ApitestForeign");
        Error = GetLastError();
        ok(Namespace == NULL, "CreatePrivateNamespaceW succeeded outside the boundary\n");
        ok(Error == ERROR_ACCESS_DENIED, "Error = %lu\n", Error);
        if (Namespace) pClosePrivateNamespace(Namespace, PRIVATE_NAMESPACE_FLAG_DESTROY);
        FreeSid(StrangerSid);
    }
    if (Foreign) pDeleteBoundaryDescriptor(Foreign);

cleanup:
    pDeleteBoundaryDescriptor(Boundary);
}
