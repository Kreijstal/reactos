/*
 * PROJECT:     ReactOS Kernel Cryptography Next Generation Provider
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Algorithm providers and object properties
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/* INCLUDES *******************************************************************/

#include "cngp.h"

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

const CNG_ALG_INFO CngAlgorithms[CngAlgMax] =
{
    /* CngAlgMd5 */
    { BCRYPT_MD5_ALGORITHM, BCRYPT_HASH_INTERFACE, 16, 64 },
    /* CngAlgSha1 */
    { BCRYPT_SHA1_ALGORITHM, BCRYPT_HASH_INTERFACE, 20, 64 },
    /* CngAlgSha256 */
    { BCRYPT_SHA256_ALGORITHM, BCRYPT_HASH_INTERFACE, 32, 64 },
    /* CngAlgSha384 */
    { BCRYPT_SHA384_ALGORITHM, BCRYPT_HASH_INTERFACE, 48, 128 },
    /* CngAlgSha512 */
    { BCRYPT_SHA512_ALGORITHM, BCRYPT_HASH_INTERFACE, 64, 128 },
    /* CngAlgAes */
    { BCRYPT_AES_ALGORITHM, BCRYPT_CIPHER_INTERFACE, 0, 0 },
    /* CngAlgChaCha20Poly1305 */
    { BCRYPT_CHACHA20_POLY1305_ALGORITHM, BCRYPT_CIPHER_INTERFACE, 0, 0 },
    /* CngAlgHkdf */
    { BCRYPT_HKDF_ALGORITHM, BCRYPT_KEY_DERIVATION_INTERFACE, 0, 0 },
    /* CngAlgRng */
    { BCRYPT_RNG_ALGORITHM, BCRYPT_RNG_INTERFACE, 0, 0 },
};

/* Algorithm pseudo-handles (Windows 10) that this provider implements */
static const struct
{
    BCRYPT_ALG_HANDLE Handle;
    CNG_ALGORITHM Algorithm;
} CngPseudoHandles[] =
{
#define PSEUDO(Handle, Id, Mode, Flags) \
    { Handle, { CNG_MAGIC_ALGORITHM, Id, Mode, (Flags) | BCRYPT_PROV_DISPATCH, NULL } }
    PSEUDO(BCRYPT_MD5_ALG_HANDLE, CngAlgMd5, CngChainNone, 0),
    PSEUDO(BCRYPT_SHA1_ALG_HANDLE, CngAlgSha1, CngChainNone, 0),
    PSEUDO(BCRYPT_SHA256_ALG_HANDLE, CngAlgSha256, CngChainNone, 0),
    PSEUDO(BCRYPT_SHA384_ALG_HANDLE, CngAlgSha384, CngChainNone, 0),
    PSEUDO(BCRYPT_SHA512_ALG_HANDLE, CngAlgSha512, CngChainNone, 0),
    PSEUDO(BCRYPT_RNG_ALG_HANDLE, CngAlgRng, CngChainNone, 0),
    PSEUDO(BCRYPT_HMAC_MD5_ALG_HANDLE, CngAlgMd5, CngChainNone, BCRYPT_ALG_HANDLE_HMAC_FLAG),
    PSEUDO(BCRYPT_HMAC_SHA1_ALG_HANDLE, CngAlgSha1, CngChainNone, BCRYPT_ALG_HANDLE_HMAC_FLAG),
    PSEUDO(BCRYPT_HMAC_SHA256_ALG_HANDLE, CngAlgSha256, CngChainNone, BCRYPT_ALG_HANDLE_HMAC_FLAG),
    PSEUDO(BCRYPT_HMAC_SHA384_ALG_HANDLE, CngAlgSha384, CngChainNone, BCRYPT_ALG_HANDLE_HMAC_FLAG),
    PSEUDO(BCRYPT_HMAC_SHA512_ALG_HANDLE, CngAlgSha512, CngChainNone, BCRYPT_ALG_HANDLE_HMAC_FLAG),
    PSEUDO(BCRYPT_AES_CBC_ALG_HANDLE, CngAlgAes, CngChainCbc, 0),
    PSEUDO(BCRYPT_AES_ECB_ALG_HANDLE, CngAlgAes, CngChainEcb, 0),
    PSEUDO(BCRYPT_AES_GCM_ALG_HANDLE, CngAlgAes, CngChainGcm, 0),
    PSEUDO(BCRYPT_HKDF_ALG_HANDLE, CngAlgHkdf, CngChainNone, 0),
    PSEUDO(BCRYPT_CHACHA20_POLY1305_ALG_HANDLE, CngAlgChaCha20Poly1305, CngChainNone, 0),
#undef PSEUDO
};

static const struct
{
    PCWSTR Name;
    CNG_CHAIN_MODE Mode;
} CngChainModes[] =
{
    { BCRYPT_CHAIN_MODE_ECB, CngChainEcb },
    { BCRYPT_CHAIN_MODE_CBC, CngChainCbc },
    { BCRYPT_CHAIN_MODE_GCM, CngChainGcm },
};

/* FUNCTIONS ******************************************************************/

BOOLEAN
CngIsPseudoHandle(
    _In_ BCRYPT_HANDLE Handle)
{
    ULONG_PTR Value = (ULONG_PTR)Handle;

    return (Value < 0x1000 && (Value & 0xF) == 1);
}

const CNG_ALGORITHM *
CngGetAlgorithm(
    _In_ BCRYPT_ALG_HANDLE Handle)
{
    const CNG_ALGORITHM *Algorithm = Handle;
    ULONG i;

    if (!Handle)
        return NULL;

    CngEnsureInitialized();

    if (CngIsPseudoHandle(Handle))
    {
        for (i = 0; i < RTL_NUMBER_OF(CngPseudoHandles); i++)
        {
            if (CngPseudoHandles[i].Handle == Handle)
                return &CngPseudoHandles[i].Algorithm;
        }
        return NULL;
    }

    if (Algorithm->Magic != CNG_MAGIC_ALGORITHM)
        return NULL;

    return Algorithm;
}

PCSYMCRYPT_MAC
CngGetHmacFromHashName(
    _In_reads_bytes_(Size) PUCHAR Name,
    _In_ ULONG Size)
{
    if (CngIsString(Name, Size, BCRYPT_SHA1_ALGORITHM))
        return SymCryptHmacSha1Algorithm;
    if (CngIsString(Name, Size, BCRYPT_SHA256_ALGORITHM))
        return SymCryptHmacSha256Algorithm;
    if (CngIsString(Name, Size, BCRYPT_SHA384_ALGORITHM))
        return SymCryptHmacSha384Algorithm;
    if (CngIsString(Name, Size, BCRYPT_SHA512_ALGORITHM))
        return SymCryptHmacSha512Algorithm;

    return NULL;
}

static
PCWSTR
CngChainModeName(
    _In_ CNG_CHAIN_MODE Mode)
{
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(CngChainModes); i++)
    {
        if (CngChainModes[i].Mode == Mode)
            return CngChainModes[i].Name;
    }
    return BCRYPT_CHAIN_MODE_NA;
}

static
NTSTATUS
CngParseChainMode(
    _In_ CNG_ALG_ID AlgId,
    _In_reads_bytes_(Size) PUCHAR Value,
    _In_ ULONG Size,
    _Out_ CNG_CHAIN_MODE *Mode)
{
    ULONG i;

    if (AlgId != CngAlgAes)
        return STATUS_NOT_SUPPORTED;

    for (i = 0; i < RTL_NUMBER_OF(CngChainModes); i++)
    {
        if (CngIsString(Value, Size, CngChainModes[i].Name))
        {
            *Mode = CngChainModes[i].Mode;
            return STATUS_SUCCESS;
        }
    }

    /* CFB and CCM are not implemented */
    DPRINT1("Unsupported chaining mode\n");
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
CngGetAlgorithmPropertyById(
    _In_ CNG_ALG_ID AlgId,
    _In_ CNG_CHAIN_MODE Mode,
    _In_ ULONG Flags,
    _In_ PCWSTR Property,
    _Out_writes_bytes_to_opt_(OutputLength, *ResultLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength)
{
    PCCNG_ALG_INFO Info = &CngAlgorithms[AlgId];
    BCRYPT_KEY_LENGTHS_STRUCT Lengths;
    ULONG Value;
    PCWSTR String;

    UNREFERENCED_PARAMETER(Flags);

    if (!wcscmp(Property, BCRYPT_ALGORITHM_NAME))
    {
        return CngCopyProperty(Info->Name,
                               (ULONG)(wcslen(Info->Name) + 1) * sizeof(WCHAR),
                               Output, OutputLength, ResultLength);
    }

    if (Info->Class == BCRYPT_HASH_INTERFACE)
    {
        if (!wcscmp(Property, BCRYPT_OBJECT_LENGTH))
            Value = CngHashObjectLength();
        else if (!wcscmp(Property, BCRYPT_HASH_LENGTH))
            Value = Info->HashLength;
        else if (!wcscmp(Property, BCRYPT_HASH_BLOCK_LENGTH))
            Value = Info->HashBlockLength;
        else
            return STATUS_NOT_SUPPORTED;

        return CngCopyProperty(&Value, sizeof(Value), Output, OutputLength, ResultLength);
    }

    if (AlgId == CngAlgRng)
        return STATUS_NOT_SUPPORTED;

    if (!wcscmp(Property, BCRYPT_OBJECT_LENGTH))
    {
        Value = CngKeyObjectLength(AlgId);
        return CngCopyProperty(&Value, sizeof(Value), Output, OutputLength, ResultLength);
    }

    if (!wcscmp(Property, BCRYPT_KEY_LENGTHS))
    {
        switch (AlgId)
        {
            case CngAlgAes:
                Lengths.dwMinLength = 128;
                Lengths.dwMaxLength = 256;
                Lengths.dwIncrement = 64;
                break;

            case CngAlgChaCha20Poly1305:
                Lengths.dwMinLength = 256;
                Lengths.dwMaxLength = 256;
                Lengths.dwIncrement = 0;
                break;

            default:
                Lengths.dwMinLength = 0;
                Lengths.dwMaxLength = CNG_HKDF_MAX_SECRET_LENGTH * 8;
                Lengths.dwIncrement = 8;
                break;
        }
        return CngCopyProperty(&Lengths, sizeof(Lengths), Output, OutputLength, ResultLength);
    }

    if (AlgId == CngAlgHkdf)
        return STATUS_NOT_SUPPORTED;

    if (!wcscmp(Property, BCRYPT_BLOCK_LENGTH))
    {
        Value = (AlgId == CngAlgAes) ? CNG_AES_BLOCK_LENGTH : 1;
        return CngCopyProperty(&Value, sizeof(Value), Output, OutputLength, ResultLength);
    }

    if (!wcscmp(Property, BCRYPT_CHAINING_MODE))
    {
        String = (AlgId == CngAlgAes) ? CngChainModeName(Mode) : BCRYPT_CHAIN_MODE_NA;
        return CngCopyProperty(String,
                               (ULONG)(wcslen(String) + 1) * sizeof(WCHAR),
                               Output, OutputLength, ResultLength);
    }

    if (!wcscmp(Property, BCRYPT_AUTH_TAG_LENGTH))
    {
        if (AlgId == CngAlgChaCha20Poly1305)
        {
            Lengths.dwMinLength = CNG_POLY1305_TAG_LENGTH;
            Lengths.dwMaxLength = CNG_POLY1305_TAG_LENGTH;
            Lengths.dwIncrement = 0;
        }
        else if (Mode == CngChainGcm)
        {
            Lengths.dwMinLength = CNG_GCM_MIN_TAG_LENGTH;
            Lengths.dwMaxLength = CNG_GCM_MAX_TAG_LENGTH;
            Lengths.dwIncrement = 1;
        }
        else
        {
            return STATUS_NOT_SUPPORTED;
        }
        return CngCopyProperty(&Lengths, sizeof(Lengths), Output, OutputLength, ResultLength);
    }

    return STATUS_NOT_SUPPORTED;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
WINAPI
BCryptOpenAlgorithmProvider(
    _Out_ BCRYPT_ALG_HANDLE *phAlgorithm,
    _In_ LPCWSTR pszAlgId,
    _In_opt_ LPCWSTR pszImplementation,
    _In_ ULONG dwFlags)
{
    PCNG_ALGORITHM Algorithm;
    PVOID Allocation;
    ULONG Id;

    if (!phAlgorithm || !pszAlgId)
        return STATUS_INVALID_PARAMETER;

    *phAlgorithm = NULL;

    /* Like on Windows this is only available at PASSIVE_LEVEL */
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_NOT_SUPPORTED;

    if (dwFlags & ~CNG_VALID_OPEN_FLAGS)
        return STATUS_INVALID_PARAMETER;

    if (pszImplementation && _wcsicmp(pszImplementation, MS_PRIMITIVE_PROVIDER) != 0)
        return STATUS_NOT_FOUND;

    for (Id = 0; Id < CngAlgMax; Id++)
    {
        if (!_wcsicmp(pszAlgId, CngAlgorithms[Id].Name))
            break;
    }

    if (Id == CngAlgMax)
    {
        DPRINT1("Algorithm %S is not supported\n", pszAlgId);
        return STATUS_NOT_FOUND;
    }

    /* Windows ignores the HMAC flag for algorithms that are not hashes */
    if (CngAlgorithms[Id].Class != BCRYPT_HASH_INTERFACE)
        dwFlags &= ~BCRYPT_ALG_HANDLE_HMAC_FLAG;

    CngEnsureInitialized();

    Algorithm = CngAllocateObject(sizeof(*Algorithm), TAG_CNG_ALGORITHM, &Allocation);
    if (!Algorithm)
        return STATUS_NO_MEMORY;

    Algorithm->Magic = CNG_MAGIC_ALGORITHM;
    Algorithm->Id = (CNG_ALG_ID)Id;
    Algorithm->Mode = (Id == CngAlgAes) ? CngChainCbc : CngChainNone;
    Algorithm->Flags = dwFlags;
    Algorithm->Allocation = Allocation;

    *phAlgorithm = Algorithm;
    return STATUS_SUCCESS;
}

NTSTATUS
WINAPI
BCryptCloseAlgorithmProvider(
    _Inout_ BCRYPT_ALG_HANDLE hAlgorithm,
    _In_ ULONG dwFlags)
{
    PCNG_ALGORITHM Algorithm = hAlgorithm;

    UNREFERENCED_PARAMETER(dwFlags);

    if (!hAlgorithm || CngIsPseudoHandle(hAlgorithm) || Algorithm->Magic != CNG_MAGIC_ALGORITHM)
        return STATUS_INVALID_HANDLE;

    Algorithm->Magic = CNG_MAGIC_FREED;
    CngFreeObject(Algorithm->Allocation, TAG_CNG_ALGORITHM);
    return STATUS_SUCCESS;
}

NTSTATUS
WINAPI
BCryptSetProperty(
    _Inout_ BCRYPT_HANDLE hObject,
    _In_ LPCWSTR pszProperty,
    _In_reads_bytes_opt_(cbInput) PUCHAR pbInput,
    _In_ ULONG cbInput,
    _In_ ULONG dwFlags)
{
    PULONG Magic = hObject;
    PCNG_ALGORITHM Algorithm;

    UNREFERENCED_PARAMETER(dwFlags);

    if (!hObject)
        return STATUS_INVALID_HANDLE;
    if (!pszProperty)
        return STATUS_INVALID_PARAMETER;

    /* Pseudo-handles are shared and cannot be modified */
    if (CngIsPseudoHandle(hObject))
        return STATUS_ACCESS_DENIED;

    switch (*Magic)
    {
        case CNG_MAGIC_ALGORITHM:
            Algorithm = hObject;
            if (!wcscmp(pszProperty, BCRYPT_CHAINING_MODE))
                return CngParseChainMode(Algorithm->Id, pbInput, cbInput, &Algorithm->Mode);
            return STATUS_NOT_SUPPORTED;

        case CNG_MAGIC_KEY:
            return CngSetKeyProperty(hObject, pszProperty, pbInput, cbInput);

        case CNG_MAGIC_HASH:
            return STATUS_NOT_SUPPORTED;

        default:
            return STATUS_INVALID_HANDLE;
    }
}

NTSTATUS
WINAPI
BCryptGetProperty(
    _In_ BCRYPT_HANDLE hObject,
    _In_ LPCWSTR pszProperty,
    _Out_writes_bytes_to_opt_(cbOutput, *pcbResult) PUCHAR pbOutput,
    _In_ ULONG cbOutput,
    _Out_ ULONG *pcbResult,
    _In_ ULONG dwFlags)
{
    const CNG_ALGORITHM *Algorithm;
    PULONG Magic = hObject;

    UNREFERENCED_PARAMETER(dwFlags);

    if (!hObject)
        return STATUS_INVALID_HANDLE;
    if (!pszProperty || !pcbResult)
        return STATUS_INVALID_PARAMETER;

    if (CngIsPseudoHandle(hObject) || *Magic == CNG_MAGIC_ALGORITHM)
    {
        Algorithm = CngGetAlgorithm(hObject);
        if (!Algorithm)
            return STATUS_INVALID_HANDLE;

        return CngGetAlgorithmPropertyById(Algorithm->Id, Algorithm->Mode, Algorithm->Flags,
                                           pszProperty, pbOutput, cbOutput, pcbResult);
    }

    switch (*Magic)
    {
        case CNG_MAGIC_KEY:
            return CngGetKeyProperty(hObject, pszProperty, pbOutput, cbOutput, pcbResult);

        case CNG_MAGIC_HASH:
            return CngGetHashProperty(hObject, pszProperty, pbOutput, cbOutput, pcbResult);

        default:
            return STATUS_INVALID_HANDLE;
    }
}
