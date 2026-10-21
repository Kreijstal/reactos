/*
 * PROJECT:     ReactOS Kernel Cryptography Next Generation Provider
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Private header for cng.sys
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#ifndef _CNGP_H_
#define _CNGP_H_

#include <ntifs.h>
#include <bcrypt.h>
#include <symcrypt.h>

/* Pool tags */
#define TAG_CNG_ALGORITHM   'aGNC'
#define TAG_CNG_KEY         'kGNC'
#define TAG_CNG_HASH        'hGNC'
#define TAG_CNG_BUFFER      'bGNC'

/* Object signatures, stored in the first field of every object */
#define CNG_MAGIC_ALGORITHM 'OGLA'
#define CNG_MAGIC_KEY       'YEK '
#define CNG_MAGIC_HASH      'HSAH'
#define CNG_MAGIC_FREED     'EERF'

/* Flags accepted by BCryptOpenAlgorithmProvider */
#define CNG_VALID_OPEN_FLAGS (BCRYPT_PROV_DISPATCH | \
                              BCRYPT_ALG_HANDLE_HMAC_FLAG | \
                              BCRYPT_HASH_REUSABLE_FLAG)

#define CNG_AES_BLOCK_LENGTH        16
#define CNG_CHACHA20_NONCE_LENGTH   12
#define CNG_CHACHA20_KEY_LENGTH     32
#define CNG_POLY1305_TAG_LENGTH     16
#define CNG_GCM_MIN_TAG_LENGTH      12
#define CNG_GCM_MAX_TAG_LENGTH      16
#define CNG_HKDF_MAX_SECRET_LENGTH  (16384 / 8)
#define CNG_MAX_HASH_LENGTH         64

/* Extra bytes that allow an object to be aligned for SymCrypt */
#define CNG_ALIGNMENT       SYMCRYPT_ALIGN_VALUE
#define CNG_ALIGN_SLACK     (CNG_ALIGNMENT - 1)

typedef enum _CNG_ALG_ID
{
    CngAlgMd5,
    CngAlgSha1,
    CngAlgSha256,
    CngAlgSha384,
    CngAlgSha512,
    CngAlgAes,
    CngAlgChaCha20Poly1305,
    CngAlgHkdf,
    CngAlgRng,
    CngAlgMax
} CNG_ALG_ID;

typedef enum _CNG_CHAIN_MODE
{
    CngChainNone,
    CngChainEcb,
    CngChainCbc,
    CngChainGcm
} CNG_CHAIN_MODE;

/* Static description of an algorithm */
typedef struct _CNG_ALG_INFO
{
    PCWSTR Name;
    ULONG Class;
    ULONG HashLength;
    ULONG HashBlockLength;
} CNG_ALG_INFO, *PCNG_ALG_INFO;
typedef const CNG_ALG_INFO *PCCNG_ALG_INFO;

/* An algorithm provider handle */
typedef struct _CNG_ALGORITHM
{
    ULONG Magic;
    CNG_ALG_ID Id;
    CNG_CHAIN_MODE Mode;
    ULONG Flags;
    PVOID Allocation;
} CNG_ALGORITHM, *PCNG_ALGORITHM;

typedef struct _CNG_HKDF_KEY
{
    PCSYMCRYPT_MAC Mac;
    BOOLEAN Finalized;
    SYMCRYPT_HKDF_EXPANDED_KEY Expanded;
} CNG_HKDF_KEY;

/* A key handle. The key secret follows the structure. */
typedef struct _CNG_KEY
{
    ULONG Magic;
    CNG_ALG_ID AlgId;
    CNG_CHAIN_MODE Mode;
    BOOLEAN CallerAllocated;
    PVOID Allocation;
    ULONG ObjectSize;
    ULONG SecretLength;
    UCHAR Iv[CNG_AES_BLOCK_LENGTH];
    union
    {
        SYMCRYPT_AES_EXPANDED_KEY Aes;
        SYMCRYPT_GCM_EXPANDED_KEY Gcm;
        CNG_HKDF_KEY Hkdf;
    } u;
    UCHAR Secret[ANYSIZE_ARRAY];
} CNG_KEY, *PCNG_KEY;

/* A hash or HMAC handle. An HMAC key is kept so that the object can be reused. */
typedef struct _CNG_HASH
{
    ULONG Magic;
    CNG_ALG_ID AlgId;
    ULONG Flags;
    BOOLEAN CallerAllocated;
    BOOLEAN Hmac;
    BOOLEAN Finished;
    PVOID Allocation;
    ULONG ObjectSize;
    PCSYMCRYPT_HASH Hash;
    union
    {
        SYMCRYPT_HASH_STATE Hash;
        struct
        {
            SYMCRYPT_HMAC_EXPANDED_KEY Key;
            SYMCRYPT_HMAC_STATE State;
        } Hmac;
    } u;
} CNG_HASH, *PCNG_HASH;

/* cng.c */
VOID
CngEnsureInitialized(VOID);

PVOID
CngAllocateObject(
    _In_ SIZE_T Size,
    _In_ ULONG Tag,
    _Out_ PVOID *Allocation);

VOID
CngFreeObject(
    _In_ PVOID Allocation,
    _In_ ULONG Tag);

BOOLEAN
CngIsString(
    _In_reads_bytes_(Size) PUCHAR Buffer,
    _In_ ULONG Size,
    _In_ PCWSTR String);

NTSTATUS
CngCopyProperty(
    _In_reads_bytes_(Length) const VOID *Value,
    _In_ ULONG Length,
    _Out_writes_bytes_to_opt_(OutputLength, *ResultLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength);

/* algorithm.c */
extern const CNG_ALG_INFO CngAlgorithms[CngAlgMax];

const CNG_ALGORITHM *
CngGetAlgorithm(
    _In_ BCRYPT_ALG_HANDLE Handle);

BOOLEAN
CngIsPseudoHandle(
    _In_ BCRYPT_HANDLE Handle);

PCSYMCRYPT_MAC
CngGetHmacFromHashName(
    _In_reads_bytes_(Size) PUCHAR Name,
    _In_ ULONG Size);

/* key.c */
PCNG_KEY
CngGetKey(
    _In_ BCRYPT_KEY_HANDLE Handle);

ULONG
CngKeyObjectLength(
    _In_ CNG_ALG_ID AlgId);

NTSTATUS
CngSetKeyProperty(
    _Inout_ PCNG_KEY Key,
    _In_ PCWSTR Property,
    _In_reads_bytes_(InputLength) PUCHAR Input,
    _In_ ULONG InputLength);

NTSTATUS
CngGetKeyProperty(
    _In_ PCNG_KEY Key,
    _In_ PCWSTR Property,
    _Out_writes_bytes_to_opt_(OutputLength, *ResultLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength);

NTSTATUS
CngGetAlgorithmPropertyById(
    _In_ CNG_ALG_ID AlgId,
    _In_ CNG_CHAIN_MODE Mode,
    _In_ ULONG Flags,
    _In_ PCWSTR Property,
    _Out_writes_bytes_to_opt_(OutputLength, *ResultLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength);

/* hash.c */
PCNG_HASH
CngGetHash(
    _In_ BCRYPT_HASH_HANDLE Handle);

ULONG
CngHashObjectLength(VOID);

NTSTATUS
CngGetHashProperty(
    _In_ PCNG_HASH Hash,
    _In_ PCWSTR Property,
    _Out_writes_bytes_to_opt_(OutputLength, *ResultLength) PUCHAR Output,
    _In_ ULONG OutputLength,
    _Out_ ULONG *ResultLength);

/* random.c */
NTSTATUS
CngInitializeRng(VOID);

VOID
CngGenerateRandom(
    _Out_writes_bytes_(Length) PUCHAR Buffer,
    _In_ SIZE_T Length);

#endif /* _CNGP_H_ */
