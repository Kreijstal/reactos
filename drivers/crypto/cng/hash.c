/*
 * PROJECT:     ReactOS Kernel Cryptography Next Generation Provider
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Hash and HMAC objects
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/* INCLUDES *******************************************************************/

#include "cngp.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ******************************************************************/

static
PCSYMCRYPT_HASH
CngGetHashDescriptor(
    _In_ CNG_ALG_ID AlgId)
{
    switch (AlgId)
    {
        case CngAlgMd5:
            return SymCryptMd5Algorithm;
        case CngAlgSha1:
            return SymCryptSha1Algorithm;
        case CngAlgSha256:
            return SymCryptSha256Algorithm;
        case CngAlgSha384:
            return SymCryptSha384Algorithm;
        case CngAlgSha512:
            return SymCryptSha512Algorithm;
        default:
            return NULL;
    }
}

PCNG_HASH
CngGetHash(
    _In_ BCRYPT_HASH_HANDLE Handle)
{
    PCNG_HASH Hash = Handle;

    if (!Handle || CngIsPseudoHandle(Handle) || Hash->Magic != CNG_MAGIC_HASH)
        return NULL;

    return Hash;
}

ULONG
CngHashObjectLength(VOID)
{
    return sizeof(CNG_HASH) + CNG_ALIGN_SLACK;
}

static
VOID
CngHashReset(
    _Inout_ PCNG_HASH Hash)
{
    if (Hash->Hmac)
        SymCryptHmacInit(&Hash->u.Hmac.State, &Hash->u.Hmac.Key);
    else
        SymCryptHashInit(Hash->Hash, &Hash->u.Hash);

    Hash->Finished = FALSE;
}

static
NTSTATUS
CngPlaceHash(
    _Out_writes_bytes_opt_(ObjectLength) PUCHAR Object,
    _In_ ULONG ObjectLength,
    _Out_ PCNG_HASH *ReturnedHash)
{
    PCNG_HASH Hash;
    PVOID Allocation = NULL;

    if (Object)
    {
        Hash = ALIGN_UP_POINTER_BY(Object, CNG_ALIGNMENT);
        if ((PUCHAR)(Hash + 1) > Object + ObjectLength || (PUCHAR)Hash < Object)
            return STATUS_BUFFER_TOO_SMALL;

        RtlZeroMemory(Hash, sizeof(*Hash));
        Hash->CallerAllocated = TRUE;
    }
    else
    {
        Hash = CngAllocateObject(sizeof(*Hash), TAG_CNG_HASH, &Allocation);
        if (!Hash)
            return STATUS_NO_MEMORY;
    }

    Hash->Allocation = Allocation;
    Hash->ObjectSize = sizeof(*Hash);
    *ReturnedHash = Hash;
    return STATUS_SUCCESS;
}

NTSTATUS
WINAPI
BCryptCreateHash(
    _Inout_ BCRYPT_ALG_HANDLE hAlgorithm,
    _Out_ BCRYPT_HASH_HANDLE *phHash,
    _Out_writes_bytes_all_opt_(cbHashObject) PUCHAR pbHashObject,
    _In_ ULONG cbHashObject,
    _In_reads_bytes_opt_(cbSecret) PUCHAR pbSecret,
    _In_ ULONG cbSecret,
    _In_ ULONG dwFlags)
{
    const CNG_ALGORITHM *Algorithm = CngGetAlgorithm(hAlgorithm);
    PCSYMCRYPT_HASH Descriptor;
    PCNG_HASH Hash;
    NTSTATUS Status;

    if (!Algorithm)
        return STATUS_INVALID_HANDLE;
    if (!phHash || (dwFlags & ~BCRYPT_HASH_REUSABLE_FLAG) || (cbSecret && !pbSecret))
        return STATUS_INVALID_PARAMETER;

    Descriptor = CngGetHashDescriptor(Algorithm->Id);
    if (!Descriptor)
        return STATUS_NOT_SUPPORTED;

    Status = CngPlaceHash(pbHashObject, cbHashObject, &Hash);
    if (!NT_SUCCESS(Status))
        return Status;

    Hash->AlgId = Algorithm->Id;
    Hash->Flags = (Algorithm->Flags | dwFlags) & BCRYPT_HASH_REUSABLE_FLAG;
    Hash->Hash = Descriptor;
    Hash->Hmac = !!(Algorithm->Flags & BCRYPT_ALG_HANDLE_HMAC_FLAG);

    if (Hash->Hmac &&
        SymCryptHmacExpandKey(Descriptor, &Hash->u.Hmac.Key,
                              pbSecret, cbSecret) != SYMCRYPT_NO_ERROR)
    {
        if (Hash->Allocation)
            CngFreeObject(Hash->Allocation, TAG_CNG_HASH);
        return STATUS_INVALID_PARAMETER;
    }

    CngHashReset(Hash);
    Hash->Magic = CNG_MAGIC_HASH;
    *phHash = Hash;
    return STATUS_SUCCESS;
}

NTSTATUS
WINAPI
BCryptHashData(
    _Inout_ BCRYPT_HASH_HANDLE hHash,
    _In_reads_bytes_(cbInput) PUCHAR pbInput,
    _In_ ULONG cbInput,
    _In_ ULONG dwFlags)
{
    PCNG_HASH Hash = CngGetHash(hHash);

    UNREFERENCED_PARAMETER(dwFlags);

    if (!Hash || Hash->Finished)
        return STATUS_INVALID_HANDLE;
    if (cbInput && !pbInput)
        return STATUS_INVALID_PARAMETER;

    if (Hash->Hmac)
        SymCryptHmacAppend(&Hash->u.Hmac.State, pbInput, cbInput);
    else
        SymCryptHashAppend(Hash->Hash, &Hash->u.Hash, pbInput, cbInput);

    return STATUS_SUCCESS;
}

NTSTATUS
WINAPI
BCryptFinishHash(
    _Inout_ BCRYPT_HASH_HANDLE hHash,
    _Out_writes_bytes_all_(cbOutput) PUCHAR pbOutput,
    _In_ ULONG cbOutput,
    _In_ ULONG dwFlags)
{
    PCNG_HASH Hash = CngGetHash(hHash);

    UNREFERENCED_PARAMETER(dwFlags);

    if (!Hash || Hash->Finished)
        return STATUS_INVALID_HANDLE;
    if (!pbOutput || cbOutput != CngAlgorithms[Hash->AlgId].HashLength)
        return STATUS_INVALID_PARAMETER;

    if (Hash->Hmac)
        SymCryptHmacResult(&Hash->u.Hmac.State, pbOutput);
    else
        SymCryptHashResult(Hash->Hash, &Hash->u.Hash, pbOutput, cbOutput);

    /* SymCrypt leaves the state ready for a new computation */
    if (Hash->Flags & BCRYPT_HASH_REUSABLE_FLAG)
        CngHashReset(Hash);
    else
        Hash->Finished = TRUE;

    return STATUS_SUCCESS;
}

NTSTATUS
WINAPI
BCryptDuplicateHash(
    _In_ BCRYPT_HASH_HANDLE hHash,
    _Out_ BCRYPT_HASH_HANDLE *phNewHash,
    _Out_writes_bytes_all_opt_(cbHashObject) PUCHAR pbHashObject,
    _In_ ULONG cbHashObject,
    _In_ ULONG dwFlags)
{
    PCNG_HASH Hash = CngGetHash(hHash);
    PCNG_HASH NewHash;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(dwFlags);

    if (!Hash)
        return STATUS_INVALID_HANDLE;
    if (!phNewHash)
        return STATUS_INVALID_PARAMETER;

    Status = CngPlaceHash(pbHashObject, cbHashObject, &NewHash);
    if (!NT_SUCCESS(Status))
        return Status;

    NewHash->AlgId = Hash->AlgId;
    NewHash->Flags = Hash->Flags;
    NewHash->Hash = Hash->Hash;
    NewHash->Hmac = Hash->Hmac;
    NewHash->Finished = Hash->Finished;

    /* The SymCrypt states point to their key, so they have to be copied with SymCrypt */
    if (Hash->Hmac)
    {
        SymCryptHmacKeyCopy(&Hash->u.Hmac.Key, &NewHash->u.Hmac.Key);
        SymCryptHmacStateCopy(&Hash->u.Hmac.State, &NewHash->u.Hmac.Key, &NewHash->u.Hmac.State);
    }
    else
    {
        SymCryptHashStateCopy(Hash->Hash, &Hash->u.Hash, &NewHash->u.Hash);
    }

    NewHash->Magic = CNG_MAGIC_HASH;
    *phNewHash = NewHash;
    return STATUS_SUCCESS;
}

NTSTATUS
WINAPI
BCryptDestroyHash(
    _Inout_ BCRYPT_HASH_HANDLE hHash)
{
    PCNG_HASH Hash = CngGetHash(hHash);
    PVOID Allocation;

    if (!Hash)
        return STATUS_INVALID_HANDLE;

    Allocation = Hash->Allocation;
    SymCryptWipe(Hash, Hash->ObjectSize);
    Hash->Magic = CNG_MAGIC_FREED;

    if (Allocation)
        CngFreeObject(Allocation, TAG_CNG_HASH);

    return STATUS_SUCCESS;
}

NTSTATUS
WINAPI
BCryptHash(
    _Inout_ BCRYPT_ALG_HANDLE hAlgorithm,
    _In_reads_bytes_opt_(cbSecret) PUCHAR pbSecret,
    _In_ ULONG cbSecret,
    _In_reads_bytes_(cbInput) PUCHAR pbInput,
    _In_ ULONG cbInput,
    _Out_writes_bytes_all_(cbOutput) PUCHAR pbOutput,
    _In_ ULONG cbOutput)
{
    BCRYPT_HASH_HANDLE Handle;
    PUCHAR Object;
    PVOID Allocation;
    NTSTATUS Status;

    /* Hash objects are too large for the kernel stack */
    Object = CngAllocateObject(CngHashObjectLength(), TAG_CNG_HASH, &Allocation);
    if (!Object)
        return STATUS_NO_MEMORY;

    Status = BCryptCreateHash(hAlgorithm, &Handle, Object, CngHashObjectLength(),
                              pbSecret, cbSecret, 0);
    if (NT_SUCCESS(Status))
    {
        Status = BCryptHashData(Handle, pbInput, cbInput, 0);
        if (NT_SUCCESS(Status))
            Status = BCryptFinishHash(Handle, pbOutput, cbOutput, 0);

        BCryptDestroyHash(Handle);
    }

    CngFreeObject(Allocation, TAG_CNG_HASH);
    return Status;
}

NTSTATUS
CngGetHashProperty(
    _In_ PCNG_HASH Hash,
    _In_ PCWSTR Property,
    _Out_writes_bytes_to_opt_(OutputLength, *ResultLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength)
{
    return CngGetAlgorithmPropertyById(Hash->AlgId, CngChainNone, Hash->Flags, Property,
                                       Output, OutputLength, ResultLength);
}
