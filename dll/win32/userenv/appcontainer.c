/*
 * PROJECT:     ReactOS User Environment
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Deterministic application container SID derivation
 */

#include "precomp.h"
#include <bcrypt.h>

#if DLL_EXPORT_VERSION >= 0x602
HRESULT WINAPI DeriveAppContainerSidFromAppContainerName(PCWSTR Name, PSID *Sid)
{
    UNICODE_STRING Input, Lower = {0};
    BCRYPT_ALG_HANDLE Algorithm = NULL;
    BCRYPT_HASH_HANDLE Hash = NULL;
    SID_IDENTIFIER_AUTHORITY Authority = SECURITY_APP_PACKAGE_AUTHORITY;
    union { BYTE Bytes[32]; DWORD Words[8]; } Digest;
    PBYTE Object = NULL;
    ULONG ObjectSize, Returned;
    NTSTATUS Status;
    HRESULT Result;
    SIZE_T Length;

    if (!Sid) return E_INVALIDARG;
    *Sid = NULL;
    if (!Name || !*Name) return E_INVALIDARG;
    Length = wcslen(Name);
    if (Length > (MAXUSHORT - sizeof(WCHAR)) / sizeof(WCHAR)) return E_INVALIDARG;
    RtlInitUnicodeString(&Input, Name);
    Status = RtlDowncaseUnicodeString(&Lower, &Input, TRUE);
    if (!NT_SUCCESS(Status)) return HRESULT_FROM_WIN32(RtlNtStatusToDosError(Status));
    Status = BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    if (!NT_SUCCESS(Status)) goto Done;
    Status = BCryptGetProperty(Algorithm, BCRYPT_OBJECT_LENGTH, (PUCHAR)&ObjectSize,
                               sizeof(ObjectSize), &Returned, 0);
    if (!NT_SUCCESS(Status)) goto Done;
    Object = HeapAlloc(GetProcessHeap(), 0, ObjectSize);
    if (!Object) { Status = STATUS_NO_MEMORY; goto Done; }
    Status = BCryptCreateHash(Algorithm, &Hash, Object, ObjectSize, NULL, 0, 0);
    if (!NT_SUCCESS(Status)) goto Done;
    Status = BCryptHashData(Hash, (PUCHAR)Lower.Buffer, Lower.Length, 0);
    if (!NT_SUCCESS(Status)) goto Done;
    Status = BCryptFinishHash(Hash, Digest.Bytes, sizeof(Digest.Bytes), 0);
    if (!NT_SUCCESS(Status)) goto Done;
    /* Windows uses seven little-endian SHA-256 words of the lowercase UTF-16
     * name, excluding its terminator, following the application package RID. */
    if (!AllocateAndInitializeSid(&Authority, SECURITY_APP_PACKAGE_RID_COUNT,
                                  SECURITY_APP_PACKAGE_BASE_RID,
                                  Digest.Words[0], Digest.Words[1], Digest.Words[2],
                                  Digest.Words[3], Digest.Words[4], Digest.Words[5],
                                  Digest.Words[6], Sid))
        Result = HRESULT_FROM_WIN32(GetLastError());
    else
        Result = S_OK;
Done:
    if (!NT_SUCCESS(Status)) Result = HRESULT_FROM_WIN32(RtlNtStatusToDosError(Status));
    if (Hash) BCryptDestroyHash(Hash);
    if (Object) HeapFree(GetProcessHeap(), 0, Object);
    if (Algorithm) BCryptCloseAlgorithmProvider(Algorithm, 0);
    RtlFreeUnicodeString(&Lower);
    return Result;
}
#endif
