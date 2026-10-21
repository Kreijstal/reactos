/*
 * PROJECT:     ReactOS Kernel Cryptography Next Generation Provider
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Symmetric encryption and decryption
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

/*
 * BCryptEncrypt and BCryptDecrypt may be called at DISPATCH_LEVEL. They do
 * not allocate and keep no per-call state in the key: AEAD state lives on the
 * stack, so callers can share an AEAD key between threads, which is what
 * Windows allows for calls that are not chained.
 */

/* INCLUDES *******************************************************************/

#include "cngp.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ******************************************************************/

static
NTSTATUS
CngValidateAuthInfo(
    _In_ PCNG_KEY Key,
    _In_opt_ PBCRYPT_AUTHENTICATED_CIPHER_MODE_INFO AuthInfo,
    _In_ ULONG Length,
    _In_ ULONG Flags)
{
    if (!AuthInfo ||
        AuthInfo->cbSize < sizeof(*AuthInfo) ||
        AuthInfo->dwInfoVersion != BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO_VERSION)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Padding makes no sense for a stream of authenticated data */
    if (Flags & BCRYPT_BLOCK_PADDING)
        return STATUS_INVALID_PARAMETER;

    if (AuthInfo->dwFlags & (BCRYPT_AUTH_MODE_CHAIN_CALLS_FLAG | BCRYPT_AUTH_MODE_IN_PROGRESS_FLAG))
    {
        DPRINT1("Chained authenticated cipher calls are not implemented\n");
        return STATUS_NOT_SUPPORTED;
    }

    if (!AuthInfo->pbNonce || !AuthInfo->cbNonce || !AuthInfo->pbTag)
        return STATUS_INVALID_PARAMETER;

    if (AuthInfo->cbAuthData && !AuthInfo->pbAuthData)
        return STATUS_INVALID_PARAMETER;

    if (Key->AlgId == CngAlgChaCha20Poly1305)
    {
        if (AuthInfo->cbNonce != CNG_CHACHA20_NONCE_LENGTH ||
            AuthInfo->cbTag != CNG_POLY1305_TAG_LENGTH)
        {
            return STATUS_INVALID_PARAMETER;
        }
        return STATUS_SUCCESS;
    }

    /* GCM takes nonces of any length, 96 bits being the efficient case */

    if (AuthInfo->cbTag < CNG_GCM_MIN_TAG_LENGTH || AuthInfo->cbTag > CNG_GCM_MAX_TAG_LENGTH)
        return STATUS_INVALID_PARAMETER;

    if (SymCryptGcmValidateParameters(SymCryptAesBlockCipher, AuthInfo->cbNonce,
                                      AuthInfo->cbAuthData, Length,
                                      AuthInfo->cbTag) != SYMCRYPT_NO_ERROR)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
CngAeadCrypt(
    _In_ PCNG_KEY Key,
    _In_ BOOLEAN Encrypt,
    _In_reads_bytes_opt_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength,
    _In_opt_ PBCRYPT_AUTHENTICATED_CIPHER_MODE_INFO AuthInfo,
    _Out_writes_bytes_to_opt_(OutputLength, *ResultLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength,
    _In_ ULONG Flags)
{
    SYMCRYPT_ERROR Error;
    NTSTATUS Status;

    Status = CngValidateAuthInfo(Key, AuthInfo, InputLength, Flags);
    if (!NT_SUCCESS(Status))
        return Status;

    *ResultLength = InputLength;
    if (!Output)
        return STATUS_SUCCESS;
    if (OutputLength < InputLength)
        return STATUS_BUFFER_TOO_SMALL;
    if (InputLength && !Input)
        return STATUS_INVALID_PARAMETER;

    if (Key->AlgId == CngAlgChaCha20Poly1305)
    {
        if (Encrypt)
        {
            Error = SymCryptChaCha20Poly1305Encrypt(Key->Secret, Key->SecretLength,
                                                    AuthInfo->pbNonce, AuthInfo->cbNonce,
                                                    AuthInfo->pbAuthData, AuthInfo->cbAuthData,
                                                    Input, Output, InputLength,
                                                    AuthInfo->pbTag, AuthInfo->cbTag);
        }
        else
        {
            Error = SymCryptChaCha20Poly1305Decrypt(Key->Secret, Key->SecretLength,
                                                    AuthInfo->pbNonce, AuthInfo->cbNonce,
                                                    AuthInfo->pbAuthData, AuthInfo->cbAuthData,
                                                    Input, Output, InputLength,
                                                    AuthInfo->pbTag, AuthInfo->cbTag);
        }
    }
    else
    {
        if (Encrypt)
        {
            SymCryptGcmEncrypt(&Key->u.Gcm, AuthInfo->pbNonce, AuthInfo->cbNonce,
                               AuthInfo->pbAuthData, AuthInfo->cbAuthData,
                               Input, Output, InputLength,
                               AuthInfo->pbTag, AuthInfo->cbTag);
            Error = SYMCRYPT_NO_ERROR;
        }
        else
        {
            Error = SymCryptGcmDecrypt(&Key->u.Gcm, AuthInfo->pbNonce, AuthInfo->cbNonce,
                                       AuthInfo->pbAuthData, AuthInfo->cbAuthData,
                                       Input, Output, InputLength,
                                       AuthInfo->pbTag, AuthInfo->cbTag);
        }
    }

    if (Error == SYMCRYPT_AUTHENTICATION_FAILURE)
        return STATUS_AUTH_TAG_MISMATCH;
    if (Error != SYMCRYPT_NO_ERROR)
        return STATUS_INVALID_PARAMETER;

    return STATUS_SUCCESS;
}

static
VOID
CngAesBlocks(
    _In_ PCNG_KEY Key,
    _In_ BOOLEAN Encrypt,
    _Inout_updates_bytes_(CNG_AES_BLOCK_LENGTH) PUCHAR Iv,
    _In_reads_bytes_(Length) PUCHAR Input,
    _Out_writes_bytes_(Length) PUCHAR Output,
    _In_ SIZE_T Length)
{
    if (!Length)
        return;

    if (Key->Mode == CngChainEcb)
    {
        if (Encrypt)
            SymCryptAesEcbEncrypt(&Key->u.Aes, Input, Output, Length);
        else
            SymCryptAesEcbDecrypt(&Key->u.Aes, Input, Output, Length);
    }
    else
    {
        if (Encrypt)
            SymCryptAesCbcEncrypt(&Key->u.Aes, Iv, Input, Output, Length);
        else
            SymCryptAesCbcDecrypt(&Key->u.Aes, Iv, Input, Output, Length);
    }
}

static
NTSTATUS
CngAesEncrypt(
    _In_ PCNG_KEY Key,
    _In_reads_bytes_opt_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength,
    _Inout_updates_bytes_(CNG_AES_BLOCK_LENGTH) PUCHAR Iv,
    _Out_writes_bytes_to_opt_(OutputLength, *ResultLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength,
    _In_ ULONG Flags)
{
    UCHAR Block[CNG_AES_BLOCK_LENGTH];
    ULONG FullLength = InputLength & ~(CNG_AES_BLOCK_LENGTH - 1);
    ULONG Remainder = InputLength - FullLength;
    ULONG Required;

    if (Flags & BCRYPT_BLOCK_PADDING)
    {
        if (FullLength > MAXULONG - CNG_AES_BLOCK_LENGTH)
            return STATUS_INVALID_PARAMETER;
        Required = FullLength + CNG_AES_BLOCK_LENGTH;
    }
    else
    {
        if (Remainder)
            return STATUS_INVALID_BUFFER_SIZE;
        Required = InputLength;
    }

    *ResultLength = Required;
    if (!Output)
        return STATUS_SUCCESS;
    if (OutputLength < Required)
        return STATUS_BUFFER_TOO_SMALL;
    if (InputLength && !Input)
        return STATUS_INVALID_PARAMETER;

    if (Flags & BCRYPT_BLOCK_PADDING)
    {
        /* PKCS#7, taken before the output may overwrite an in-place input */
        RtlCopyMemory(Block, Input + FullLength, Remainder);
        RtlFillMemory(Block + Remainder, CNG_AES_BLOCK_LENGTH - Remainder,
                      (UCHAR)(CNG_AES_BLOCK_LENGTH - Remainder));
    }

    CngAesBlocks(Key, TRUE, Iv, Input, Output, FullLength);

    if (Flags & BCRYPT_BLOCK_PADDING)
    {
        CngAesBlocks(Key, TRUE, Iv, Block, Output + FullLength, CNG_AES_BLOCK_LENGTH);
        SymCryptWipeKnownSize(Block, sizeof(Block));
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
CngAesDecrypt(
    _In_ PCNG_KEY Key,
    _In_reads_bytes_opt_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength,
    _Inout_updates_bytes_(CNG_AES_BLOCK_LENGTH) PUCHAR Iv,
    _Out_writes_bytes_to_opt_(OutputLength, *ResultLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength,
    _In_ ULONG Flags)
{
    UCHAR Block[CNG_AES_BLOCK_LENGTH];
    ULONG FullLength;
    ULONG Pad, i;
    UCHAR Bad = 0;

    if (InputLength & (CNG_AES_BLOCK_LENGTH - 1))
        return STATUS_INVALID_BUFFER_SIZE;

    if (!(Flags & BCRYPT_BLOCK_PADDING))
    {
        *ResultLength = InputLength;
        if (!Output)
            return STATUS_SUCCESS;
        if (OutputLength < InputLength)
            return STATUS_BUFFER_TOO_SMALL;
        if (InputLength && !Input)
            return STATUS_INVALID_PARAMETER;

        CngAesBlocks(Key, FALSE, Iv, Input, Output, InputLength);
        return STATUS_SUCCESS;
    }

    if (InputLength < CNG_AES_BLOCK_LENGTH || !Input)
        return STATUS_INVALID_PARAMETER;

    /* The exact length is only known after decryption */
    *ResultLength = InputLength;
    if (!Output)
        return STATUS_SUCCESS;

    FullLength = InputLength - CNG_AES_BLOCK_LENGTH;
    if (OutputLength < FullLength)
        return STATUS_BUFFER_TOO_SMALL;

    CngAesBlocks(Key, FALSE, Iv, Input, Output, FullLength);
    CngAesBlocks(Key, FALSE, Iv, Input + FullLength, Block, CNG_AES_BLOCK_LENGTH);

    Pad = Block[CNG_AES_BLOCK_LENGTH - 1];
    if (Pad == 0 || Pad > CNG_AES_BLOCK_LENGTH)
        Bad = 1;
    else
    {
        for (i = CNG_AES_BLOCK_LENGTH - Pad; i < CNG_AES_BLOCK_LENGTH; i++)
            Bad |= (UCHAR)(Block[i] ^ Pad);
    }

    if (Bad)
    {
        SymCryptWipeKnownSize(Block, sizeof(Block));
        return STATUS_DATA_ERROR;
    }

    *ResultLength = FullLength + CNG_AES_BLOCK_LENGTH - Pad;
    if (OutputLength < *ResultLength)
    {
        SymCryptWipeKnownSize(Block, sizeof(Block));
        return STATUS_BUFFER_TOO_SMALL;
    }

    RtlCopyMemory(Output + FullLength, Block, CNG_AES_BLOCK_LENGTH - Pad);
    SymCryptWipeKnownSize(Block, sizeof(Block));
    return STATUS_SUCCESS;
}

static
NTSTATUS
CngCrypt(
    _In_ BOOLEAN Encrypt,
    _Inout_ BCRYPT_KEY_HANDLE hKey,
    _In_reads_bytes_opt_(cbInput) PUCHAR pbInput,
    _In_ ULONG cbInput,
    _In_opt_ VOID *pPaddingInfo,
    _Inout_updates_bytes_opt_(cbIV) PUCHAR pbIV,
    _In_ ULONG cbIV,
    _Out_writes_bytes_to_opt_(cbOutput, *pcbResult) PUCHAR pbOutput,
    _In_ ULONG cbOutput,
    _Out_ ULONG *pcbResult,
    _In_ ULONG dwFlags)
{
    PCNG_KEY Key = CngGetKey(hKey);
    PUCHAR Iv;

    if (!Key)
        return STATUS_INVALID_HANDLE;
    if (!pcbResult)
        return STATUS_INVALID_PARAMETER;

    *pcbResult = 0;

    if (dwFlags & ~BCRYPT_BLOCK_PADDING)
        return STATUS_INVALID_PARAMETER;

    if (Key->AlgId == CngAlgChaCha20Poly1305 ||
        (Key->AlgId == CngAlgAes && Key->Mode == CngChainGcm))
    {
        return CngAeadCrypt(Key, Encrypt, pbInput, cbInput, pPaddingInfo,
                            pbOutput, cbOutput, pcbResult, dwFlags);
    }

    if (Key->AlgId != CngAlgAes)
        return STATUS_NOT_SUPPORTED;

    if (Key->Mode == CngChainEcb)
    {
        Iv = Key->Iv;
    }
    else if (pbIV)
    {
        if (cbIV != CNG_AES_BLOCK_LENGTH)
            return STATUS_INVALID_PARAMETER;
        Iv = pbIV;
    }
    else
    {
        /* Without an IV the key's own chaining value is used and updated */
        Iv = Key->Iv;
    }

    if (Encrypt)
        return CngAesEncrypt(Key, pbInput, cbInput, Iv, pbOutput, cbOutput, pcbResult, dwFlags);

    return CngAesDecrypt(Key, pbInput, cbInput, Iv, pbOutput, cbOutput, pcbResult, dwFlags);
}

NTSTATUS
WINAPI
BCryptEncrypt(
    _Inout_ BCRYPT_KEY_HANDLE hKey,
    _In_reads_bytes_opt_(cbInput) PUCHAR pbInput,
    _In_ ULONG cbInput,
    _In_opt_ VOID *pPaddingInfo,
    _Inout_updates_bytes_opt_(cbIV) PUCHAR pbIV,
    _In_ ULONG cbIV,
    _Out_writes_bytes_to_opt_(cbOutput, *pcbResult) PUCHAR pbOutput,
    _In_ ULONG cbOutput,
    _Out_ ULONG *pcbResult,
    _In_ ULONG dwFlags)
{
    return CngCrypt(TRUE, hKey, pbInput, cbInput, pPaddingInfo, pbIV, cbIV,
                    pbOutput, cbOutput, pcbResult, dwFlags);
}

NTSTATUS
WINAPI
BCryptDecrypt(
    _Inout_ BCRYPT_KEY_HANDLE hKey,
    _In_reads_bytes_opt_(cbInput) PUCHAR pbInput,
    _In_ ULONG cbInput,
    _In_opt_ VOID *pPaddingInfo,
    _Inout_updates_bytes_opt_(cbIV) PUCHAR pbIV,
    _In_ ULONG cbIV,
    _Out_writes_bytes_to_opt_(cbOutput, *pcbResult) PUCHAR pbOutput,
    _In_ ULONG cbOutput,
    _Out_ ULONG *pcbResult,
    _In_ ULONG dwFlags)
{
    return CngCrypt(FALSE, hKey, pbInput, cbInput, pPaddingInfo, pbIV, cbIV,
                    pbOutput, cbOutput, pcbResult, dwFlags);
}
