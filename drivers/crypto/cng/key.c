/*
 * PROJECT:     ReactOS Kernel Cryptography Next Generation Provider
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Symmetric keys and key derivation
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/* INCLUDES *******************************************************************/

#include "cngp.h"

#define NDEBUG
#include <debug.h>

/* Secret that still fits a key object of the size reported by BCRYPT_OBJECT_LENGTH */
#define CNG_KEY_INLINE_SECRET_LENGTH 64

/* FUNCTIONS ******************************************************************/

PCNG_KEY
CngGetKey(
    _In_ BCRYPT_KEY_HANDLE Handle)
{
    PCNG_KEY Key = Handle;

    if (!Handle || CngIsPseudoHandle(Handle) || Key->Magic != CNG_MAGIC_KEY)
        return NULL;

    return Key;
}

static
ULONG
CngMaxInlineSecret(
    _In_ CNG_ALG_ID AlgId)
{
    if (AlgId == CngAlgAes || AlgId == CngAlgChaCha20Poly1305)
        return 32;

    return CNG_KEY_INLINE_SECRET_LENGTH;
}

ULONG
CngKeyObjectLength(
    _In_ CNG_ALG_ID AlgId)
{
    return FIELD_OFFSET(CNG_KEY, Secret) + CngMaxInlineSecret(AlgId) + CNG_ALIGN_SLACK;
}

static
NTSTATUS
CngValidateSecretLength(
    _In_ CNG_ALG_ID AlgId,
    _In_ ULONG SecretLength)
{
    switch (AlgId)
    {
        case CngAlgAes:
            if (SecretLength != 16 && SecretLength != 24 && SecretLength != 32)
                return STATUS_INVALID_PARAMETER;
            return STATUS_SUCCESS;

        case CngAlgChaCha20Poly1305:
            if (SecretLength != CNG_CHACHA20_KEY_LENGTH)
                return STATUS_INVALID_PARAMETER;
            return STATUS_SUCCESS;

        case CngAlgHkdf:
            if (SecretLength > CNG_HKDF_MAX_SECRET_LENGTH)
                return STATUS_INVALID_PARAMETER;
            return STATUS_SUCCESS;

        default:
            return STATUS_NOT_SUPPORTED;
    }
}

static
NTSTATUS
CngExpandKey(
    _Inout_ PCNG_KEY Key)
{
    SYMCRYPT_ERROR Error = SYMCRYPT_NO_ERROR;

    if (Key->AlgId != CngAlgAes)
        return STATUS_SUCCESS;

    if (Key->Mode == CngChainGcm)
    {
        Error = SymCryptGcmExpandKey(&Key->u.Gcm, SymCryptAesBlockCipher,
                                     Key->Secret, Key->SecretLength);
    }
    else
    {
        Error = SymCryptAesExpandKey(&Key->u.Aes, Key->Secret, Key->SecretLength);
    }

    return (Error == SYMCRYPT_NO_ERROR) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

static
NTSTATUS
CngCreateKey(
    _In_ CNG_ALG_ID AlgId,
    _In_ CNG_CHAIN_MODE Mode,
    _In_reads_bytes_(SecretLength) PUCHAR Secret,
    _In_ ULONG SecretLength,
    _Out_writes_bytes_opt_(ObjectLength) PUCHAR Object,
    _In_ ULONG ObjectLength,
    _Out_ PCNG_KEY *ReturnedKey)
{
    PCNG_KEY Key;
    PVOID Allocation = NULL;
    SIZE_T Size;
    NTSTATUS Status;

    *ReturnedKey = NULL;

    Status = CngValidateSecretLength(AlgId, SecretLength);
    if (!NT_SUCCESS(Status))
        return Status;

    if (SecretLength && !Secret)
        return STATUS_INVALID_PARAMETER;

    Size = FIELD_OFFSET(CNG_KEY, Secret) + SecretLength;

    if (Object)
    {
        /* The caller owns the memory, which must stay valid until BCryptDestroyKey */
        Key = ALIGN_UP_POINTER_BY(Object, CNG_ALIGNMENT);
        if ((PUCHAR)Key + Size > Object + ObjectLength || (PUCHAR)Key < Object)
            return STATUS_BUFFER_TOO_SMALL;

        RtlZeroMemory(Key, Size);
        Key->CallerAllocated = TRUE;
    }
    else
    {
        Key = CngAllocateObject(Size, TAG_CNG_KEY, &Allocation);
        if (!Key)
            return STATUS_NO_MEMORY;
    }

    Key->AlgId = AlgId;
    Key->Mode = Mode;
    Key->Allocation = Allocation;
    Key->ObjectSize = (ULONG)Size;
    Key->SecretLength = SecretLength;
    if (SecretLength)
        RtlCopyMemory(Key->Secret, Secret, SecretLength);

    Status = CngExpandKey(Key);
    if (!NT_SUCCESS(Status))
    {
        SymCryptWipe(Key, Size);
        if (Allocation)
            CngFreeObject(Allocation, TAG_CNG_KEY);
        return Status;
    }

    Key->Magic = CNG_MAGIC_KEY;
    *ReturnedKey = Key;
    return STATUS_SUCCESS;
}

NTSTATUS
WINAPI
BCryptGenerateSymmetricKey(
    _Inout_ BCRYPT_ALG_HANDLE hAlgorithm,
    _Out_ BCRYPT_KEY_HANDLE *phKey,
    _Out_writes_bytes_all_opt_(cbKeyObject) PUCHAR pbKeyObject,
    _In_ ULONG cbKeyObject,
    _In_reads_bytes_(cbSecret) PUCHAR pbSecret,
    _In_ ULONG cbSecret,
    _In_ ULONG dwFlags)
{
    const CNG_ALGORITHM *Algorithm = CngGetAlgorithm(hAlgorithm);
    PCNG_KEY Key;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(dwFlags);

    if (!Algorithm)
        return STATUS_INVALID_HANDLE;
    if (!phKey)
        return STATUS_INVALID_PARAMETER;

    Status = CngCreateKey(Algorithm->Id, Algorithm->Mode, pbSecret, cbSecret,
                          pbKeyObject, cbKeyObject, &Key);
    if (NT_SUCCESS(Status))
        *phKey = Key;

    return Status;
}

NTSTATUS
WINAPI
BCryptImportKey(
    _In_ BCRYPT_ALG_HANDLE hAlgorithm,
    _In_opt_ BCRYPT_KEY_HANDLE hImportKey,
    _In_ LPCWSTR pszBlobType,
    _Out_ BCRYPT_KEY_HANDLE *phKey,
    _Out_writes_bytes_all_opt_(cbKeyObject) PUCHAR pbKeyObject,
    _In_ ULONG cbKeyObject,
    _In_reads_bytes_(cbInput) PUCHAR pbInput,
    _In_ ULONG cbInput,
    _In_ ULONG dwFlags)
{
    const CNG_ALGORITHM *Algorithm = CngGetAlgorithm(hAlgorithm);
    PBCRYPT_KEY_DATA_BLOB_HEADER Header = (PBCRYPT_KEY_DATA_BLOB_HEADER)pbInput;
    PCNG_KEY Key;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(dwFlags);

    if (!Algorithm)
        return STATUS_INVALID_HANDLE;
    if (!pszBlobType || !phKey || !pbInput)
        return STATUS_INVALID_PARAMETER;

    /* Only plain key data blobs are supported */
    if (hImportKey || wcscmp(pszBlobType, BCRYPT_KEY_DATA_BLOB) != 0)
        return STATUS_NOT_SUPPORTED;

    if (cbInput < sizeof(*Header) ||
        Header->dwMagic != BCRYPT_KEY_DATA_BLOB_MAGIC ||
        Header->dwVersion != BCRYPT_KEY_DATA_BLOB_VERSION1 ||
        Header->cbKeyData > cbInput - sizeof(*Header))
    {
        return STATUS_INVALID_PARAMETER;
    }

    Status = CngCreateKey(Algorithm->Id, Algorithm->Mode, (PUCHAR)(Header + 1),
                          Header->cbKeyData, pbKeyObject, cbKeyObject, &Key);
    if (NT_SUCCESS(Status))
        *phKey = Key;

    return Status;
}

NTSTATUS
WINAPI
BCryptExportKey(
    _In_ BCRYPT_KEY_HANDLE hKey,
    _In_opt_ BCRYPT_KEY_HANDLE hExportKey,
    _In_ LPCWSTR pszBlobType,
    _Out_writes_bytes_to_opt_(cbOutput, *pcbResult) PUCHAR pbOutput,
    _In_ ULONG cbOutput,
    _Out_ ULONG *pcbResult,
    _In_ ULONG dwFlags)
{
    PCNG_KEY Key = CngGetKey(hKey);
    PBCRYPT_KEY_DATA_BLOB_HEADER Header = (PBCRYPT_KEY_DATA_BLOB_HEADER)pbOutput;
    ULONG Length;

    UNREFERENCED_PARAMETER(dwFlags);

    if (!Key)
        return STATUS_INVALID_HANDLE;
    if (!pszBlobType || !pcbResult)
        return STATUS_INVALID_PARAMETER;

    if (hExportKey || wcscmp(pszBlobType, BCRYPT_KEY_DATA_BLOB) != 0 || Key->AlgId == CngAlgHkdf)
        return STATUS_NOT_SUPPORTED;

    Length = sizeof(*Header) + Key->SecretLength;
    *pcbResult = Length;
    if (!pbOutput)
        return STATUS_SUCCESS;
    if (cbOutput < Length)
        return STATUS_BUFFER_TOO_SMALL;

    Header->dwMagic = BCRYPT_KEY_DATA_BLOB_MAGIC;
    Header->dwVersion = BCRYPT_KEY_DATA_BLOB_VERSION1;
    Header->cbKeyData = Key->SecretLength;
    RtlCopyMemory(Header + 1, Key->Secret, Key->SecretLength);
    return STATUS_SUCCESS;
}

NTSTATUS
WINAPI
BCryptDuplicateKey(
    _In_ BCRYPT_KEY_HANDLE hKey,
    _Out_ BCRYPT_KEY_HANDLE *phNewKey,
    _Out_writes_bytes_all_opt_(cbKeyObject) PUCHAR pbKeyObject,
    _In_ ULONG cbKeyObject,
    _In_ ULONG dwFlags)
{
    PCNG_KEY Key = CngGetKey(hKey);
    PCNG_KEY NewKey;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(dwFlags);

    if (!Key)
        return STATUS_INVALID_HANDLE;
    if (!phNewKey)
        return STATUS_INVALID_PARAMETER;

    Status = CngCreateKey(Key->AlgId, Key->Mode, Key->Secret, Key->SecretLength,
                          pbKeyObject, cbKeyObject, &NewKey);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlCopyMemory(NewKey->Iv, Key->Iv, sizeof(NewKey->Iv));

    /* An expanded HKDF key holds no pointers into itself */
    if (Key->AlgId == CngAlgHkdf)
        NewKey->u.Hkdf = Key->u.Hkdf;

    *phNewKey = NewKey;
    return STATUS_SUCCESS;
}

NTSTATUS
WINAPI
BCryptDestroyKey(
    _Inout_ BCRYPT_KEY_HANDLE hKey)
{
    PCNG_KEY Key = CngGetKey(hKey);
    PVOID Allocation;

    if (!Key)
        return STATUS_INVALID_HANDLE;

    Allocation = Key->Allocation;
    SymCryptWipe(Key, Key->ObjectSize);
    Key->Magic = CNG_MAGIC_FREED;

    if (Allocation)
        CngFreeObject(Allocation, TAG_CNG_KEY);

    return STATUS_SUCCESS;
}

static
NTSTATUS
CngHkdfFinalize(
    _Inout_ PCNG_KEY Key,
    _In_ BOOLEAN IsPrk,
    _In_reads_bytes_opt_(SaltLength) PUCHAR Salt,
    _In_ ULONG SaltLength)
{
    SYMCRYPT_ERROR Error;

    if (!Key->u.Hkdf.Mac || Key->u.Hkdf.Finalized)
        return STATUS_INVALID_PARAMETER;

    if (SaltLength && !Salt)
        return STATUS_INVALID_PARAMETER;

    if (IsPrk)
    {
        Error = SymCryptHkdfPrkExpandKey(&Key->u.Hkdf.Expanded, Key->u.Hkdf.Mac,
                                         Key->Secret, Key->SecretLength);
    }
    else
    {
        Error = SymCryptHkdfExpandKey(&Key->u.Hkdf.Expanded, Key->u.Hkdf.Mac,
                                      Key->Secret, Key->SecretLength, Salt, SaltLength);
    }

    if (Error != SYMCRYPT_NO_ERROR)
        return STATUS_INVALID_PARAMETER;

    Key->u.Hkdf.Finalized = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS
CngSetKeyProperty(
    _Inout_ PCNG_KEY Key,
    _In_ PCWSTR Property,
    _In_reads_bytes_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength)
{
    CNG_CHAIN_MODE Mode;

    if (Key->AlgId == CngAlgHkdf)
    {
        if (!wcscmp(Property, BCRYPT_HKDF_HASH_ALGORITHM))
        {
            if (Key->u.Hkdf.Finalized)
                return STATUS_INVALID_PARAMETER;

            Key->u.Hkdf.Mac = CngGetHmacFromHashName(Input, InputLength);
            return Key->u.Hkdf.Mac ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
        }

        if (!wcscmp(Property, BCRYPT_HKDF_SALT_AND_FINALIZE))
            return CngHkdfFinalize(Key, FALSE, Input, InputLength);

        if (!wcscmp(Property, BCRYPT_HKDF_PRK_AND_FINALIZE))
            return CngHkdfFinalize(Key, TRUE, NULL, 0);

        return STATUS_NOT_SUPPORTED;
    }

    if (Key->AlgId != CngAlgAes)
        return STATUS_NOT_SUPPORTED;

    if (!wcscmp(Property, BCRYPT_CHAINING_MODE))
    {
        if (CngIsString(Input, InputLength, BCRYPT_CHAIN_MODE_ECB))
            Mode = CngChainEcb;
        else if (CngIsString(Input, InputLength, BCRYPT_CHAIN_MODE_CBC))
            Mode = CngChainCbc;
        else if (CngIsString(Input, InputLength, BCRYPT_CHAIN_MODE_GCM))
            Mode = CngChainGcm;
        else
            return STATUS_NOT_SUPPORTED;

        Key->Mode = Mode;
        return CngExpandKey(Key);
    }

    if (!wcscmp(Property, BCRYPT_INITIALIZATION_VECTOR))
    {
        if (!Input || InputLength != sizeof(Key->Iv))
            return STATUS_INVALID_PARAMETER;

        RtlCopyMemory(Key->Iv, Input, sizeof(Key->Iv));
        return STATUS_SUCCESS;
    }

    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
CngGetKeyProperty(
    _In_ PCNG_KEY Key,
    _In_ PCWSTR Property,
    _Out_writes_bytes_to_opt_(OutputLength, *ResultLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength)
{
    ULONG Value;

    if (!wcscmp(Property, BCRYPT_KEY_LENGTH) || !wcscmp(Property, BCRYPT_KEY_STRENGTH))
    {
        Value = Key->SecretLength * 8;
        return CngCopyProperty(&Value, sizeof(Value), Output, OutputLength, ResultLength);
    }

    if (Key->AlgId == CngAlgAes && !wcscmp(Property, BCRYPT_INITIALIZATION_VECTOR))
        return CngCopyProperty(Key->Iv, sizeof(Key->Iv), Output, OutputLength, ResultLength);

    return CngGetAlgorithmPropertyById(Key->AlgId, Key->Mode, 0, Property,
                                       Output, OutputLength, ResultLength);
}

NTSTATUS
WINAPI
BCryptKeyDerivation(
    _In_ BCRYPT_KEY_HANDLE hKey,
    _In_opt_ BCryptBufferDesc *pParameterList,
    _Out_writes_bytes_to_(cbDerivedKey, *pcbResult) PUCHAR pbDerivedKey,
    _In_ ULONG cbDerivedKey,
    _Out_ ULONG *pcbResult,
    _In_ ULONG dwFlags)
{
    PCNG_KEY Key = CngGetKey(hKey);
    PUCHAR Info = NULL;
    ULONG InfoLength = 0;
    ULONG MaxLength;
    ULONG i;

    UNREFERENCED_PARAMETER(dwFlags);

    if (!Key)
        return STATUS_INVALID_HANDLE;
    if (!pbDerivedKey || !pcbResult)
        return STATUS_INVALID_PARAMETER;

    if (Key->AlgId != CngAlgHkdf)
        return STATUS_NOT_SUPPORTED;

    if (!Key->u.Hkdf.Finalized)
        return STATUS_INVALID_PARAMETER;

    /*
     * Like Windows, use the first info buffer and ignore everything else.
     * The version of the descriptor is not checked either.
     */
    if (pParameterList)
    {
        if (pParameterList->cBuffers && !pParameterList->pBuffers)
            return STATUS_INVALID_PARAMETER;

        for (i = 0; i < pParameterList->cBuffers; i++)
        {
            BCryptBuffer *Buffer = &pParameterList->pBuffers[i];

            if (Buffer->BufferType != KDF_HKDF_INFO)
                continue;
            if (Buffer->cbBuffer && !Buffer->pvBuffer)
                return STATUS_INVALID_PARAMETER;

            Info = Buffer->pvBuffer;
            InfoLength = Buffer->cbBuffer;
            break;
        }
    }

    /* RFC 5869: L <= 255 * HashLen */
    MaxLength = 255 * (ULONG)SymCryptHashResultSize(*Key->u.Hkdf.Mac->ppHashAlgorithm);
    if (cbDerivedKey == 0)
        return STATUS_INVALID_PARAMETER;
    if (cbDerivedKey > MaxLength)
        return STATUS_UNSUCCESSFUL;

    if (SymCryptHkdfDerive(&Key->u.Hkdf.Expanded, Info, InfoLength,
                           pbDerivedKey, cbDerivedKey) != SYMCRYPT_NO_ERROR)
    {
        return STATUS_INTERNAL_ERROR;
    }

    *pcbResult = cbDerivedKey;
    return STATUS_SUCCESS;
}
