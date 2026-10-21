/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel-mode test for the CNG provider (cng.sys)
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <kmt_test.h>
#include <bcrypt.h>

#include "Cng.h"

/* Known answers: FIPS 180-2, RFC 4231, RFC 5869, the GCM specification
 * (McGrew/Viega test cases 3, 4, 5, 13, 14, 16), RFC 8439 section 2.8.2 and
 * SP 800-38A. The padded CBC vector was computed independently. */

static const UCHAR Sha256Abc[32] =
{
    0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde,
    0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
    0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
};

static const UCHAR Sha256Empty[32] =
{
    0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4, 0xc8,
    0x99, 0x6f, 0xb9, 0x24, 0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
    0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55,
};

static const UCHAR Sha256Long[32] =
{
    0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8, 0xe5, 0xc0, 0x26, 0x93,
    0x0c, 0x3e, 0x60, 0x39, 0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67,
    0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1,
};

static const UCHAR HmacSha256Jefe[32] =
{
    0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e, 0x6a, 0x04, 0x24, 0x26,
    0x08, 0x95, 0x75, 0xc7, 0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27, 0x39, 0x83,
    0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43,
};

static const UCHAR HkdfIkm[22] =
{
    0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
    0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
};

static const UCHAR HkdfSalt[13] =
{
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
    0x0c,
};

static const UCHAR HkdfInfo[10] =
{
    0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9,
};

static const UCHAR HkdfPrk[32] =
{
    0x07, 0x77, 0x09, 0x36, 0x2c, 0x2e, 0x32, 0xdf, 0x0d, 0xdc, 0x3f, 0x0d,
    0xc4, 0x7b, 0xba, 0x63, 0x90, 0xb6, 0xc7, 0x3b, 0xb5, 0x0f, 0x9c, 0x31,
    0x22, 0xec, 0x84, 0x4a, 0xd7, 0xc2, 0xb3, 0xe5,
};

static const UCHAR HkdfOkm1[42] =
{
    0x3c, 0xb2, 0x5f, 0x25, 0xfa, 0xac, 0xd5, 0x7a, 0x90, 0x43, 0x4f, 0x64,
    0xd0, 0x36, 0x2f, 0x2a, 0x2d, 0x2d, 0x0a, 0x90, 0xcf, 0x1a, 0x5a, 0x4c,
    0x5d, 0xb0, 0x2d, 0x56, 0xec, 0xc4, 0xc5, 0xbf, 0x34, 0x00, 0x72, 0x08,
    0xd5, 0xb8, 0x87, 0x18, 0x58, 0x65,
};

static const UCHAR HkdfOkm3[42] =
{
    0x8d, 0xa4, 0xe7, 0x75, 0xa5, 0x63, 0xc1, 0x8f, 0x71, 0x5f, 0x80, 0x2a,
    0x06, 0x3c, 0x5a, 0x31, 0xb8, 0xa1, 0x1f, 0x5c, 0x5e, 0xe1, 0x87, 0x9e,
    0xc3, 0x45, 0x4e, 0x5f, 0x3c, 0x73, 0x8d, 0x2d, 0x9d, 0x20, 0x13, 0x95,
    0xfa, 0xa4, 0xb6, 0x1a, 0x96, 0xc8,
};

static const UCHAR GcmKey[16] =
{
    0xfe, 0xff, 0xe9, 0x92, 0x86, 0x65, 0x73, 0x1c, 0x6d, 0x6a, 0x8f, 0x94,
    0x67, 0x30, 0x83, 0x08,
};

static const UCHAR GcmIv[12] =
{
    0xca, 0xfe, 0xba, 0xbe, 0xfa, 0xce, 0xdb, 0xad, 0xde, 0xca, 0xf8, 0x88,
};

static const UCHAR GcmPlain[64] =
{
    0xd9, 0x31, 0x32, 0x25, 0xf8, 0x84, 0x06, 0xe5, 0xa5, 0x59, 0x09, 0xc5,
    0xaf, 0xf5, 0x26, 0x9a, 0x86, 0xa7, 0xa9, 0x53, 0x15, 0x34, 0xf7, 0xda,
    0x2e, 0x4c, 0x30, 0x3d, 0x8a, 0x31, 0x8a, 0x72, 0x1c, 0x3c, 0x0c, 0x95,
    0x95, 0x68, 0x09, 0x53, 0x2f, 0xcf, 0x0e, 0x24, 0x49, 0xa6, 0xb5, 0x25,
    0xb1, 0x6a, 0xed, 0xf5, 0xaa, 0x0d, 0xe6, 0x57, 0xba, 0x63, 0x7b, 0x39,
    0x1a, 0xaf, 0xd2, 0x55,
};

static const UCHAR GcmAad[20] =
{
    0xfe, 0xed, 0xfa, 0xce, 0xde, 0xad, 0xbe, 0xef, 0xfe, 0xed, 0xfa, 0xce,
    0xde, 0xad, 0xbe, 0xef, 0xab, 0xad, 0xda, 0xd2,
};

static const UCHAR GcmCipher3[64] =
{
    0x42, 0x83, 0x1e, 0xc2, 0x21, 0x77, 0x74, 0x24, 0x4b, 0x72, 0x21, 0xb7,
    0x84, 0xd0, 0xd4, 0x9c, 0xe3, 0xaa, 0x21, 0x2f, 0x2c, 0x02, 0xa4, 0xe0,
    0x35, 0xc1, 0x7e, 0x23, 0x29, 0xac, 0xa1, 0x2e, 0x21, 0xd5, 0x14, 0xb2,
    0x54, 0x66, 0x93, 0x1c, 0x7d, 0x8f, 0x6a, 0x5a, 0xac, 0x84, 0xaa, 0x05,
    0x1b, 0xa3, 0x0b, 0x39, 0x6a, 0x0a, 0xac, 0x97, 0x3d, 0x58, 0xe0, 0x91,
    0x47, 0x3f, 0x59, 0x85,
};

static const UCHAR GcmTag3[16] =
{
    0x4d, 0x5c, 0x2a, 0xf3, 0x27, 0xcd, 0x64, 0xa6, 0x2c, 0xf3, 0x5a, 0xbd,
    0x2b, 0xa6, 0xfa, 0xb4,
};

static const UCHAR GcmIv5[8] =
{
    0xca, 0xfe, 0xba, 0xbe, 0xfa, 0xce, 0xdb, 0xad,
};

static const UCHAR GcmCipher5[60] =
{
    0x61, 0x35, 0x3b, 0x4c, 0x28, 0x06, 0x93, 0x4a, 0x77, 0x7f, 0xf5, 0x1f,
    0xa2, 0x2a, 0x47, 0x55, 0x69, 0x9b, 0x2a, 0x71, 0x4f, 0xcd, 0xc6, 0xf8,
    0x37, 0x66, 0xe5, 0xf9, 0x7b, 0x6c, 0x74, 0x23, 0x73, 0x80, 0x69, 0x00,
    0xe4, 0x9f, 0x24, 0xb2, 0x2b, 0x09, 0x75, 0x44, 0xd4, 0x89, 0x6b, 0x42,
    0x49, 0x89, 0xb5, 0xe1, 0xeb, 0xac, 0x0f, 0x07, 0xc2, 0x3f, 0x45, 0x98,
};

static const UCHAR GcmTag5[16] =
{
    0x36, 0x12, 0xd2, 0xe7, 0x9e, 0x3b, 0x07, 0x85, 0x56, 0x1b, 0xe1, 0x4a,
    0xac, 0xa2, 0xfc, 0xcb,
};

static const UCHAR GcmTag4[16] =
{
    0x5b, 0xc9, 0x4f, 0xbc, 0x32, 0x21, 0xa5, 0xdb, 0x94, 0xfa, 0xe9, 0x5a,
    0xe7, 0x12, 0x1a, 0x47,
};

static const UCHAR GcmCipher16[60] =
{
    0x52, 0x2d, 0xc1, 0xf0, 0x99, 0x56, 0x7d, 0x07, 0xf4, 0x7f, 0x37, 0xa3,
    0x2a, 0x84, 0x42, 0x7d, 0x64, 0x3a, 0x8c, 0xdc, 0xbf, 0xe5, 0xc0, 0xc9,
    0x75, 0x98, 0xa2, 0xbd, 0x25, 0x55, 0xd1, 0xaa, 0x8c, 0xb0, 0x8e, 0x48,
    0x59, 0x0d, 0xbb, 0x3d, 0xa7, 0xb0, 0x8b, 0x10, 0x56, 0x82, 0x88, 0x38,
    0xc5, 0xf6, 0x1e, 0x63, 0x93, 0xba, 0x7a, 0x0a, 0xbc, 0xc9, 0xf6, 0x62,
};

static const UCHAR GcmTag16[16] =
{
    0x76, 0xfc, 0x6e, 0xce, 0x0f, 0x4e, 0x17, 0x68, 0xcd, 0xdf, 0x88, 0x53,
    0xbb, 0x2d, 0x55, 0x1b,
};

static const UCHAR GcmTag13[16] =
{
    0x53, 0x0f, 0x8a, 0xfb, 0xc7, 0x45, 0x36, 0xb9, 0xa9, 0x63, 0xb4, 0xf1,
    0xc4, 0xcb, 0x73, 0x8b,
};

static const UCHAR GcmCipher14[16] =
{
    0xce, 0xa7, 0x40, 0x3d, 0x4d, 0x60, 0x6b, 0x6e, 0x07, 0x4e, 0xc5, 0xd3,
    0xba, 0xf3, 0x9d, 0x18,
};

static const UCHAR GcmTag14[16] =
{
    0xd0, 0xd1, 0xc8, 0xa7, 0x99, 0x99, 0x6b, 0xf0, 0x26, 0x5b, 0x98, 0xb5,
    0xd4, 0x8a, 0xb9, 0x19,
};

static const UCHAR ChaChaKey[32] =
{
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b,
    0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
};

static const UCHAR ChaChaNonce[12] =
{
    0x07, 0x00, 0x00, 0x00, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
};

static const UCHAR ChaChaAad[12] =
{
    0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
};

static const UCHAR ChaChaPlain[114] =
{
    0x4c, 0x61, 0x64, 0x69, 0x65, 0x73, 0x20, 0x61, 0x6e, 0x64, 0x20, 0x47,
    0x65, 0x6e, 0x74, 0x6c, 0x65, 0x6d, 0x65, 0x6e, 0x20, 0x6f, 0x66, 0x20,
    0x74, 0x68, 0x65, 0x20, 0x63, 0x6c, 0x61, 0x73, 0x73, 0x20, 0x6f, 0x66,
    0x20, 0x27, 0x39, 0x39, 0x3a, 0x20, 0x49, 0x66, 0x20, 0x49, 0x20, 0x63,
    0x6f, 0x75, 0x6c, 0x64, 0x20, 0x6f, 0x66, 0x66, 0x65, 0x72, 0x20, 0x79,
    0x6f, 0x75, 0x20, 0x6f, 0x6e, 0x6c, 0x79, 0x20, 0x6f, 0x6e, 0x65, 0x20,
    0x74, 0x69, 0x70, 0x20, 0x66, 0x6f, 0x72, 0x20, 0x74, 0x68, 0x65, 0x20,
    0x66, 0x75, 0x74, 0x75, 0x72, 0x65, 0x2c, 0x20, 0x73, 0x75, 0x6e, 0x73,
    0x63, 0x72, 0x65, 0x65, 0x6e, 0x20, 0x77, 0x6f, 0x75, 0x6c, 0x64, 0x20,
    0x62, 0x65, 0x20, 0x69, 0x74, 0x2e,
};

static const UCHAR ChaChaCipher[114] =
{
    0xd3, 0x1a, 0x8d, 0x34, 0x64, 0x8e, 0x60, 0xdb, 0x7b, 0x86, 0xaf, 0xbc,
    0x53, 0xef, 0x7e, 0xc2, 0xa4, 0xad, 0xed, 0x51, 0x29, 0x6e, 0x08, 0xfe,
    0xa9, 0xe2, 0xb5, 0xa7, 0x36, 0xee, 0x62, 0xd6, 0x3d, 0xbe, 0xa4, 0x5e,
    0x8c, 0xa9, 0x67, 0x12, 0x82, 0xfa, 0xfb, 0x69, 0xda, 0x92, 0x72, 0x8b,
    0x1a, 0x71, 0xde, 0x0a, 0x9e, 0x06, 0x0b, 0x29, 0x05, 0xd6, 0xa5, 0xb6,
    0x7e, 0xcd, 0x3b, 0x36, 0x92, 0xdd, 0xbd, 0x7f, 0x2d, 0x77, 0x8b, 0x8c,
    0x98, 0x03, 0xae, 0xe3, 0x28, 0x09, 0x1b, 0x58, 0xfa, 0xb3, 0x24, 0xe4,
    0xfa, 0xd6, 0x75, 0x94, 0x55, 0x85, 0x80, 0x8b, 0x48, 0x31, 0xd7, 0xbc,
    0x3f, 0xf4, 0xde, 0xf0, 0x8e, 0x4b, 0x7a, 0x9d, 0xe5, 0x76, 0xd2, 0x65,
    0x86, 0xce, 0xc6, 0x4b, 0x61, 0x16,
};

static const UCHAR ChaChaTag[16] =
{
    0x1a, 0xe1, 0x0b, 0x59, 0x4f, 0x09, 0xe2, 0x6a, 0x7e, 0x90, 0x2e, 0xcb,
    0xd0, 0x60, 0x06, 0x91,
};

static const UCHAR AesKey[16] =
{
    0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88,
    0x09, 0xcf, 0x4f, 0x3c,
};

static const UCHAR AesIv[16] =
{
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
    0x0c, 0x0d, 0x0e, 0x0f,
};

static const UCHAR AesPlain[32] =
{
    0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11,
    0x73, 0x93, 0x17, 0x2a, 0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c,
    0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51,
};

static const UCHAR AesCbcCipher[32] =
{
    0x76, 0x49, 0xab, 0xac, 0x81, 0x19, 0xb2, 0x46, 0xce, 0xe9, 0x8e, 0x9b,
    0x12, 0xe9, 0x19, 0x7d, 0x50, 0x86, 0xcb, 0x9b, 0x50, 0x72, 0x19, 0xee,
    0x95, 0xdb, 0x11, 0x3a, 0x91, 0x76, 0x78, 0xb2,
};

static const UCHAR AesEcbCipher[16] =
{
    0x3a, 0xd7, 0x7b, 0xb4, 0x0d, 0x7a, 0x36, 0x60, 0xa8, 0x9e, 0xca, 0xf3,
    0x24, 0x66, 0xef, 0x97,
};

static const UCHAR AesCbcPadCipher[32] =
{
    0x2f, 0xd8, 0x2f, 0x20, 0xe9, 0xb7, 0xbb, 0x08, 0x28, 0x0c, 0x59, 0xcb,
    0x14, 0x3b, 0xb4, 0xe4, 0xeb, 0x19, 0xcd, 0x59, 0xd2, 0x7c, 0xf4, 0x52,
    0xed, 0xe7, 0x99, 0x04, 0xec, 0x36, 0xf4, 0xa3,
};

#define TAG_CNG_TEST 'tGNC'

static
NTSTATUS
AeadCrypt(
    _In_ BCRYPT_KEY_HANDLE Key,
    _In_ BOOLEAN Encrypt,
    _In_reads_bytes_(12) const UCHAR *Nonce,
    _In_reads_bytes_opt_(AadLength) const UCHAR *Aad,
    _In_ ULONG AadLength,
    _In_reads_bytes_(Length) PUCHAR Input,
    _Out_writes_bytes_(Length) PUCHAR Output,
    _In_ ULONG Length,
    _Inout_updates_bytes_(TagLength) PUCHAR Tag,
    _In_ ULONG TagLength,
    _Out_ PULONG Result)
{
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO AuthInfo;

    BCRYPT_INIT_AUTH_MODE_INFO(AuthInfo);
    AuthInfo.pbNonce = (PUCHAR)Nonce;
    AuthInfo.cbNonce = 12;
    AuthInfo.pbAuthData = (PUCHAR)Aad;
    AuthInfo.cbAuthData = AadLength;
    AuthInfo.pbTag = Tag;
    AuthInfo.cbTag = TagLength;

    *Result = 0xdeadbeef;
    if (Encrypt)
        return BCryptEncrypt(Key, Input, Length, &AuthInfo, NULL, 0, Output, Length, Result, 0);
    return BCryptDecrypt(Key, Input, Length, &AuthInfo, NULL, 0, Output, Length, Result, 0);
}

static
VOID
TestHash(VOID)
{
    BCRYPT_ALG_HANDLE Alg;
    BCRYPT_HASH_HANDLE Hash, Copy;
    UCHAR Digest[32];
    ULONG Value, Size;
    NTSTATUS Status;

    Status = BCryptOpenAlgorithmProvider(&Alg, BCRYPT_SHA256_ALGORITHM, NULL, BCRYPT_PROV_DISPATCH);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Status = BCryptGetProperty(Alg, BCRYPT_HASH_LENGTH, (PUCHAR)&Value, sizeof(Value), &Size, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(Value, 32UL);
    ok_eq_ulong(Size, (ULONG)sizeof(Value));

    Status = BCryptCreateHash(Alg, &Hash, NULL, 0, NULL, 0, BCRYPT_HASH_REUSABLE_FLAG);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = BCryptHashData(Hash, (PUCHAR)"ab", 2, 0);
        ok_eq_hex(Status, STATUS_SUCCESS);

        /* A copy continues independently */
        Status = BCryptDuplicateHash(Hash, &Copy, NULL, 0, 0);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Status = BCryptHashData(Hash, (PUCHAR)"c", 1, 0);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Status = BCryptFinishHash(Hash, Digest, sizeof(Digest), 0);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(RtlEqualMemory(Digest, Sha256Abc, sizeof(Digest)), "SHA-256(\"abc\") mismatch\n");

        Status = BCryptHashData(Copy, (PUCHAR)"c", 1, 0);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Status = BCryptFinishHash(Copy, Digest, sizeof(Digest), 0);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(RtlEqualMemory(Digest, Sha256Abc, sizeof(Digest)), "duplicated SHA-256 mismatch\n");
        Status = BCryptDestroyHash(Copy);
        ok_eq_hex(Status, STATUS_SUCCESS);

        /* Reusable: starts over after finishing */
        Status = BCryptFinishHash(Hash, Digest, sizeof(Digest), 0);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(RtlEqualMemory(Digest, Sha256Empty, sizeof(Digest)), "SHA-256(\"\") mismatch\n");

        Status = BCryptFinishHash(Hash, Digest, sizeof(Digest) - 1, 0);
        ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
        Status = BCryptDestroyHash(Hash);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }

    Status = BCryptCloseAlgorithmProvider(Alg, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* One-shot through the pseudo-handle */
    RtlZeroMemory(Digest, sizeof(Digest));
    Status = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, NULL, 0,
                        (PUCHAR)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
                        Digest, sizeof(Digest));
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(RtlEqualMemory(Digest, Sha256Long, sizeof(Digest)), "SHA-256(448 bits) mismatch\n");

    /* HMAC-SHA256, RFC 4231 test case 2 */
    RtlZeroMemory(Digest, sizeof(Digest));
    Status = BCryptHash(BCRYPT_HMAC_SHA256_ALG_HANDLE, (PUCHAR)"Jefe", 4,
                        (PUCHAR)"what do ya want for nothing?", 28, Digest, sizeof(Digest));
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(RtlEqualMemory(Digest, HmacSha256Jefe, sizeof(Digest)), "HMAC-SHA256 mismatch\n");
}

static
VOID
HkdfDerive(
    _In_ BCRYPT_ALG_HANDLE Alg,
    _In_ BOOLEAN UsePrk,
    _In_reads_bytes_opt_(SaltLength) const UCHAR *Salt,
    _In_ ULONG SaltLength,
    _In_reads_bytes_opt_(InfoLength) const UCHAR *Info,
    _In_ ULONG InfoLength,
    _In_reads_bytes_(42) const UCHAR *Expected)
{
    BCRYPT_KEY_HANDLE Key;
    BCryptBuffer Buffer;
    BCryptBufferDesc Desc;
    UCHAR Okm[42];
    PUCHAR Large;
    ULONG Result;
    NTSTATUS Status;

    if (UsePrk)
    {
        Status = BCryptGenerateSymmetricKey(Alg, &Key, NULL, 0,
                                            (PUCHAR)HkdfPrk, sizeof(HkdfPrk), 0);
    }
    else
    {
        Status = BCryptGenerateSymmetricKey(Alg, &Key, NULL, 0,
                                            (PUCHAR)HkdfIkm, sizeof(HkdfIkm), 0);
    }
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    /* Not finalized yet */
    Status = BCryptKeyDerivation(Key, NULL, Okm, sizeof(Okm), &Result, 0);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    /* What ovpn-dco-win does: sizeof() of the wide literal includes the NUL */
    Status = BCryptSetProperty(Key, BCRYPT_HKDF_HASH_ALGORITHM, (PUCHAR)BCRYPT_SHA256_ALGORITHM,
                               sizeof(BCRYPT_SHA256_ALGORITHM), 0);
    ok_eq_hex(Status, STATUS_SUCCESS);

    if (UsePrk)
        Status = BCryptSetProperty(Key, BCRYPT_HKDF_PRK_AND_FINALIZE, NULL, 0, 0);
    else
        Status = BCryptSetProperty(Key, BCRYPT_HKDF_SALT_AND_FINALIZE, (PUCHAR)Salt, SaltLength, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);

    Buffer.cbBuffer = InfoLength;
    Buffer.BufferType = KDF_HKDF_INFO;
    Buffer.pvBuffer = (PVOID)Info;
    Desc.ulVersion = BCRYPTBUFFER_VERSION;
    Desc.cBuffers = 1;
    Desc.pBuffers = &Buffer;

    RtlZeroMemory(Okm, sizeof(Okm));
    Result = 0;
    Status = BCryptKeyDerivation(Key, Info ? &Desc : NULL, Okm, sizeof(Okm), &Result, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(Result, (ULONG)sizeof(Okm));
    ok(RtlEqualMemory(Okm, Expected, sizeof(Okm)), "HKDF-SHA256 output mismatch (prk=%d)\n",
       UsePrk);

    /* Only the first info buffer counts, other buffer types are ignored */
    if (Info)
    {
        BCryptBuffer Buffers[3];

        Buffers[0].cbBuffer = 4;
        Buffers[0].BufferType = KDF_LABEL;
        Buffers[0].pvBuffer = "abcd";
        Buffers[1] = Buffer;
        Buffers[2].cbBuffer = 3;
        Buffers[2].BufferType = KDF_HKDF_INFO;
        Buffers[2].pvBuffer = "abc";
        Desc.cBuffers = RTL_NUMBER_OF(Buffers);
        Desc.pBuffers = Buffers;

        RtlZeroMemory(Okm, sizeof(Okm));
        Status = BCryptKeyDerivation(Key, &Desc, Okm, sizeof(Okm), &Result, 0);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(RtlEqualMemory(Okm, Expected, sizeof(Okm)), "HKDF with several buffers mismatch\n");
    }

    Status = BCryptKeyDerivation(Key, NULL, Okm, 0, &Result, 0);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    /* L > 255 * HashLen */
    Large = ExAllocatePoolWithTag(NonPagedPool, 255 * 32 + 1, TAG_CNG_TEST);
    ok(Large != NULL, "allocation failed\n");
    if (Large)
    {
        Status = BCryptKeyDerivation(Key, NULL, Large, 255 * 32 + 1, &Result, 0);
        ok_eq_hex(Status, STATUS_UNSUCCESSFUL);
        Status = BCryptKeyDerivation(Key, NULL, Large, 255 * 32, &Result, 0);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ExFreePoolWithTag(Large, TAG_CNG_TEST);
    }

    Status = BCryptDestroyKey(Key);
    ok_eq_hex(Status, STATUS_SUCCESS);
}

static
VOID
TestHkdf(VOID)
{
    BCRYPT_ALG_HANDLE Alg;
    NTSTATUS Status;

    Status = BCryptOpenAlgorithmProvider(&Alg, BCRYPT_HKDF_ALGORITHM, NULL, BCRYPT_PROV_DISPATCH);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    /* RFC 5869 test case 1, extract + expand and expand only */
    HkdfDerive(Alg, FALSE, HkdfSalt, sizeof(HkdfSalt), HkdfInfo, sizeof(HkdfInfo), HkdfOkm1);
    HkdfDerive(Alg, TRUE, NULL, 0, HkdfInfo, sizeof(HkdfInfo), HkdfOkm1);

    /* RFC 5869 test case 3: no salt, no info */
    HkdfDerive(Alg, FALSE, NULL, 0, NULL, 0, HkdfOkm3);

    Status = BCryptCloseAlgorithmProvider(Alg, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* The pseudo-handle works as well */
    HkdfDerive(BCRYPT_HKDF_ALG_HANDLE, TRUE, NULL, 0, HkdfInfo, sizeof(HkdfInfo), HkdfOkm1);
}

static
VOID
GcmCase(
    _In_ BCRYPT_ALG_HANDLE Alg,
    _In_reads_bytes_(KeyLength) const UCHAR *KeyData,
    _In_ ULONG KeyLength,
    _In_reads_bytes_(12) const UCHAR *Nonce,
    _In_reads_bytes_opt_(AadLength) const UCHAR *Aad,
    _In_ ULONG AadLength,
    _In_reads_bytes_opt_(Length) const UCHAR *Plain,
    _In_reads_bytes_opt_(Length) const UCHAR *Cipher,
    _In_ ULONG Length,
    _In_reads_bytes_(16) const UCHAR *ExpectedTag,
    _In_ PCSTR Name)
{
    BCRYPT_KEY_HANDLE Key;
    UCHAR Buffer[64];
    UCHAR Tag[16];
    ULONG Result;
    BOOLEAN PlainIsZero;
    NTSTATUS Status;

    ASSERT(Length <= sizeof(Buffer));

    /* A wiped buffer cannot be told apart from an all-zero plaintext */
    RtlZeroMemory(Buffer, sizeof(Buffer));
    PlainIsZero = !Length || RtlEqualMemory(Plain, Buffer, Length);

    Status = BCryptGenerateSymmetricKey(Alg, &Key, NULL, 0, (PUCHAR)KeyData, KeyLength, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    /* Encrypt in place, like ovpn-dco-win */
    RtlCopyMemory(Buffer, Plain, Length);
    RtlZeroMemory(Tag, sizeof(Tag));
    Status = AeadCrypt(Key, TRUE, Nonce, Aad, AadLength, Buffer, Buffer, Length, Tag, 16, &Result);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(Result, Length);
    ok(!Length || RtlEqualMemory(Buffer, Cipher, Length), "%s: ciphertext mismatch\n", Name);
    ok(RtlEqualMemory(Tag, ExpectedTag, 16), "%s: tag mismatch\n", Name);

    /* Decrypt in place */
    Status = AeadCrypt(Key, FALSE, Nonce, Aad, AadLength, Buffer, Buffer, Length, Tag, 16, &Result);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(!Length || RtlEqualMemory(Buffer, Plain, Length), "%s: plaintext mismatch\n", Name);

    /* A truncated 12-byte tag is the prefix of the full one */
    RtlCopyMemory(Buffer, Plain, Length);
    Status = AeadCrypt(Key, TRUE, Nonce, Aad, AadLength, Buffer, Buffer, Length, Tag, 12, &Result);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(RtlEqualMemory(Tag, ExpectedTag, 12), "%s: 12-byte tag mismatch\n", Name);

    /* Tampered tag */
    if (Length)
        RtlCopyMemory(Buffer, Cipher, Length);
    RtlCopyMemory(Tag, ExpectedTag, 16);
    Tag[15] ^= 0x01;
    Status = AeadCrypt(Key, FALSE, Nonce, Aad, AadLength, Buffer, Buffer, Length, Tag, 16, &Result);
    ok_eq_hex(Status, STATUS_AUTH_TAG_MISMATCH);

    /* Tampered ciphertext, the plaintext must not be released */
    if (Length)
    {
        RtlCopyMemory(Buffer, Cipher, Length);
        Buffer[0] ^= 0x80;
        RtlCopyMemory(Tag, ExpectedTag, 16);
        Status = AeadCrypt(Key, FALSE, Nonce, Aad, AadLength,
                           Buffer, Buffer, Length, Tag, 16, &Result);
        ok_eq_hex(Status, STATUS_AUTH_TAG_MISMATCH);
        ok(!RtlEqualMemory(Buffer + 1, Plain + 1, Length - 1) || PlainIsZero,
           "%s: plaintext leaked\n", Name);
    }

    Status = BCryptDestroyKey(Key);
    ok_eq_hex(Status, STATUS_SUCCESS);
}

static
VOID
TestAesGcm(VOID)
{
    BCRYPT_ALG_HANDLE Alg;
    BCRYPT_KEY_HANDLE Key;
    BCRYPT_AUTH_TAG_LENGTHS_STRUCT TagLengths;
    UCHAR Buffer[16], Tag[16];
    UCHAR Nonce[16] = { 0 };
    PUCHAR KeyObject;
    ULONG ObjectLength, Size, Result;
    NTSTATUS Status;

    /* The same calls as ovpn-dco-win's OvpnCryptoInitAlgHandles */
    Status = BCryptOpenAlgorithmProvider(&Alg, BCRYPT_AES_ALGORITHM, NULL, BCRYPT_PROV_DISPATCH);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Status = BCryptSetProperty(Alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                               sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    ok_eq_hex(Status, STATUS_SUCCESS);

    Status = BCryptGetProperty(Alg, BCRYPT_AUTH_TAG_LENGTH, (PUCHAR)&TagLengths,
                               sizeof(TagLengths), &Size, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok_eq_ulong(TagLengths.dwMinLength, 12UL);
    ok_eq_ulong(TagLengths.dwMaxLength, 16UL);

    GcmCase(Alg, GcmKey, sizeof(GcmKey), GcmIv, NULL, 0, GcmPlain, GcmCipher3,
            sizeof(GcmPlain), GcmTag3, "AES-128-GCM TC3");
    GcmCase(Alg, GcmKey, sizeof(GcmKey), GcmIv, GcmAad, sizeof(GcmAad), GcmPlain, GcmCipher3,
            60, GcmTag4, "AES-128-GCM TC4");
    {
        UCHAR Key256[32];

        RtlCopyMemory(Key256, GcmKey, 16);
        RtlCopyMemory(Key256 + 16, GcmKey, 16);
        GcmCase(Alg, Key256, sizeof(Key256), GcmIv, GcmAad, sizeof(GcmAad), GcmPlain,
                GcmCipher16, 60, GcmTag16, "AES-256-GCM TC16");

        RtlZeroMemory(Key256, sizeof(Key256));
        GcmCase(Alg, Key256, sizeof(Key256), Nonce, NULL, 0, NULL, NULL, 0, GcmTag13,
                "AES-256-GCM TC13");
        RtlZeroMemory(Buffer, sizeof(Buffer));
        GcmCase(Alg, Key256, sizeof(Key256), Nonce, NULL, 0, Buffer, GcmCipher14, 16,
                GcmTag14, "AES-256-GCM TC14");
    }

    /* Bad parameters */
    Status = BCryptGenerateSymmetricKey(Alg, &Key, NULL, 0, (PUCHAR)GcmKey, 15, 0);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    /* A key object supplied by the caller */
    Status = BCryptGetProperty(Alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&ObjectLength,
                               sizeof(ObjectLength), &Size, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    KeyObject = ExAllocatePoolWithTag(NonPagedPool, ObjectLength, TAG_CNG_TEST);
    ok(KeyObject != NULL, "allocation failed\n");
    if (KeyObject)
    {
        Status = BCryptGenerateSymmetricKey(Alg, &Key, KeyObject, ObjectLength - 1,
                                            (PUCHAR)GcmKey, sizeof(GcmKey), 0);
        ok(Status == STATUS_BUFFER_TOO_SMALL || Status == STATUS_SUCCESS,
           "Status = 0x%08lx\n", Status);
        if (NT_SUCCESS(Status))
            BCryptDestroyKey(Key);

        Status = BCryptGenerateSymmetricKey(Alg, &Key, KeyObject, ObjectLength,
                                            (PUCHAR)GcmKey, sizeof(GcmKey), 0);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            ok((PUCHAR)Key >= KeyObject && (PUCHAR)Key < KeyObject + ObjectLength,
               "key %p is not inside the caller's object %p\n", Key, KeyObject);

            RtlCopyMemory(Buffer, GcmPlain, 16);
            Status = AeadCrypt(Key, TRUE, GcmIv, NULL, 0, Buffer, Buffer, 16, Tag, 16, &Result);
            ok_eq_hex(Status, STATUS_SUCCESS);
            ok(RtlEqualMemory(Buffer, GcmCipher3, 16), "caller object: ciphertext mismatch\n");

            {
                BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO AuthInfo;
                UCHAR Data[60];

                /* Test case 5, a 64-bit nonce */
                BCRYPT_INIT_AUTH_MODE_INFO(AuthInfo);
                AuthInfo.pbNonce = (PUCHAR)GcmIv5;
                AuthInfo.cbNonce = sizeof(GcmIv5);
                AuthInfo.pbAuthData = (PUCHAR)GcmAad;
                AuthInfo.cbAuthData = sizeof(GcmAad);
                AuthInfo.pbTag = Tag;
                AuthInfo.cbTag = 16;
                RtlCopyMemory(Data, GcmPlain, sizeof(Data));
                Status = BCryptEncrypt(Key, Data, sizeof(Data), &AuthInfo, NULL, 0,
                                       Data, sizeof(Data), &Result, 0);
                ok_eq_hex(Status, STATUS_SUCCESS);
                ok(RtlEqualMemory(Data, GcmCipher5, sizeof(Data)), "TC5: ciphertext mismatch\n");
                ok(RtlEqualMemory(Tag, GcmTag5, sizeof(Tag)), "TC5: tag mismatch\n");

                /* Query the output size */
                Result = 0;
                Status = BCryptEncrypt(Key, Data, sizeof(Data), &AuthInfo, NULL, 0,
                                       NULL, 0, &Result, 0);
                ok_eq_hex(Status, STATUS_SUCCESS);
                ok_eq_ulong(Result, (ULONG)sizeof(Data));
                Status = BCryptEncrypt(Key, Data, sizeof(Data), &AuthInfo, NULL, 0,
                                       Data, 8, &Result, 0);
                ok_eq_hex(Status, STATUS_BUFFER_TOO_SMALL);

                BCRYPT_INIT_AUTH_MODE_INFO(AuthInfo);
                AuthInfo.pbNonce = Nonce;
                AuthInfo.cbNonce = 0;
                AuthInfo.pbTag = Tag;
                AuthInfo.cbTag = 16;
                Status = BCryptEncrypt(Key, Buffer, 16, &AuthInfo, NULL, 0, Buffer, 16, &Result, 0);
                ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

                AuthInfo.cbNonce = 12;
                AuthInfo.dwInfoVersion = 2;
                Status = BCryptEncrypt(Key, Buffer, 16, &AuthInfo, NULL, 0, Buffer, 16, &Result, 0);
                ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
                AuthInfo.dwInfoVersion = BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO_VERSION;

                AuthInfo.cbNonce = 12;
                AuthInfo.cbTag = 11;
                Status = BCryptEncrypt(Key, Buffer, 16, &AuthInfo, NULL, 0, Buffer, 16, &Result, 0);
                ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

                /* No auth info at all */
                Status = BCryptEncrypt(Key, Buffer, 16, NULL, NULL, 0, Buffer, 16, &Result, 0);
                ok_eq_hex(Status, STATUS_INVALID_PARAMETER);
            }

            Status = BCryptDestroyKey(Key);
            ok_eq_hex(Status, STATUS_SUCCESS);
        }
        ExFreePoolWithTag(KeyObject, TAG_CNG_TEST);
    }

    Status = BCryptCloseAlgorithmProvider(Alg, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);

    /* Pseudo-handles are read-only */
    Status = BCryptSetProperty(BCRYPT_AES_GCM_ALG_HANDLE, BCRYPT_CHAINING_MODE,
                               (PUCHAR)BCRYPT_CHAIN_MODE_CBC, sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
    ok_eq_hex(Status, STATUS_ACCESS_DENIED);
    GcmCase(BCRYPT_AES_GCM_ALG_HANDLE, GcmKey, sizeof(GcmKey), GcmIv, NULL, 0, GcmPlain,
            GcmCipher3, sizeof(GcmPlain), GcmTag3, "AES-128-GCM pseudo-handle");
}

static
VOID
TestChaCha20Poly1305(VOID)
{
    BCRYPT_ALG_HANDLE Alg;
    BCRYPT_KEY_HANDLE Key;
    UCHAR Buffer[sizeof(ChaChaPlain)];
    UCHAR Tag[16];
    ULONG Result;
    NTSTATUS Status;

    Status = BCryptOpenAlgorithmProvider(&Alg, BCRYPT_CHACHA20_POLY1305_ALGORITHM, NULL,
                                         BCRYPT_PROV_DISPATCH);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    Status = BCryptGenerateSymmetricKey(Alg, &Key, NULL, 0, (PUCHAR)ChaChaKey, 16, 0);
    ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

    Status = BCryptGenerateSymmetricKey(Alg, &Key, NULL, 0,
                                        (PUCHAR)ChaChaKey, sizeof(ChaChaKey), 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        RtlCopyMemory(Buffer, ChaChaPlain, sizeof(Buffer));
        Status = AeadCrypt(Key, TRUE, ChaChaNonce, ChaChaAad, sizeof(ChaChaAad),
                           Buffer, Buffer, sizeof(Buffer), Tag, sizeof(Tag), &Result);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulong(Result, (ULONG)sizeof(Buffer));
        ok(RtlEqualMemory(Buffer, ChaChaCipher, sizeof(Buffer)), "ChaCha20 ciphertext mismatch\n");
        ok(RtlEqualMemory(Tag, ChaChaTag, sizeof(Tag)), "Poly1305 tag mismatch\n");

        Status = AeadCrypt(Key, FALSE, ChaChaNonce, ChaChaAad, sizeof(ChaChaAad),
                           Buffer, Buffer, sizeof(Buffer), Tag, sizeof(Tag), &Result);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(RtlEqualMemory(Buffer, ChaChaPlain, sizeof(Buffer)), "ChaCha20 plaintext mismatch\n");

        /* Tampered tag */
        RtlCopyMemory(Buffer, ChaChaCipher, sizeof(Buffer));
        Tag[0] ^= 0x01;
        Status = AeadCrypt(Key, FALSE, ChaChaNonce, ChaChaAad, sizeof(ChaChaAad),
                           Buffer, Buffer, sizeof(Buffer), Tag, sizeof(Tag), &Result);
        ok_eq_hex(Status, STATUS_AUTH_TAG_MISMATCH);

        /* Tampered AAD */
        RtlCopyMemory(Tag, ChaChaTag, sizeof(Tag));
        Status = AeadCrypt(Key, FALSE, ChaChaNonce, ChaChaAad, sizeof(ChaChaAad) - 1,
                           Buffer, Buffer, sizeof(Buffer), Tag, sizeof(Tag), &Result);
        ok_eq_hex(Status, STATUS_AUTH_TAG_MISMATCH);

        /* Poly1305 tags are always 16 bytes */
        Status = AeadCrypt(Key, TRUE, ChaChaNonce, NULL, 0, Buffer, Buffer, sizeof(Buffer),
                           Tag, 12, &Result);
        ok_eq_hex(Status, STATUS_INVALID_PARAMETER);

        Status = BCryptDestroyKey(Key);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }

    Status = BCryptCloseAlgorithmProvider(Alg, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
}

static
VOID
TestAesCbcEcb(VOID)
{
    BCRYPT_ALG_HANDLE Alg;
    BCRYPT_KEY_HANDLE Key;
    UCHAR Iv[16], Buffer[48];
    static const UCHAR Message[] = "ReactOS CNG padding!";
    ULONG Result;
    NTSTATUS Status;

    Status = BCryptOpenAlgorithmProvider(&Alg, BCRYPT_AES_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (!NT_SUCCESS(Status))
        return;

    /* CBC is the default */
    Status = BCryptGenerateSymmetricKey(Alg, &Key, NULL, 0, (PUCHAR)AesKey, sizeof(AesKey), 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        RtlCopyMemory(Iv, AesIv, sizeof(Iv));
        Status = BCryptEncrypt(Key, (PUCHAR)AesPlain, sizeof(AesPlain), NULL, Iv, sizeof(Iv),
                               Buffer, sizeof(Buffer), &Result, 0);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulong(Result, (ULONG)sizeof(AesPlain));
        ok(RtlEqualMemory(Buffer, AesCbcCipher, sizeof(AesCbcCipher)), "AES-CBC mismatch\n");
        ok(RtlEqualMemory(Iv, AesCbcCipher + 16, 16), "IV was not updated\n");

        /* Not a multiple of the block size without padding */
        Status = BCryptEncrypt(Key, (PUCHAR)Message, 20, NULL, Iv, sizeof(Iv),
                               Buffer, sizeof(Buffer), &Result, 0);
        ok_eq_hex(Status, STATUS_INVALID_BUFFER_SIZE);

        /* PKCS#7 padding */
        RtlCopyMemory(Iv, AesIv, sizeof(Iv));
        Status = BCryptEncrypt(Key, (PUCHAR)Message, 20, NULL, Iv, sizeof(Iv),
                               NULL, 0, &Result, BCRYPT_BLOCK_PADDING);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulong(Result, 32UL);
        Status = BCryptEncrypt(Key, (PUCHAR)Message, 20, NULL, Iv, sizeof(Iv),
                               Buffer, sizeof(Buffer), &Result, BCRYPT_BLOCK_PADDING);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulong(Result, 32UL);
        ok(RtlEqualMemory(Buffer, AesCbcPadCipher, sizeof(AesCbcPadCipher)),
           "padded CBC mismatch\n");

        RtlCopyMemory(Iv, AesIv, sizeof(Iv));
        Status = BCryptDecrypt(Key, Buffer, 32, NULL, Iv, sizeof(Iv),
                               Buffer, sizeof(Buffer), &Result, BCRYPT_BLOCK_PADDING);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok_eq_ulong(Result, 20UL);
        ok(RtlEqualMemory(Buffer, Message, 20), "padded CBC round trip mismatch\n");

        /* Switch the key to ECB */
        Status = BCryptSetProperty(Key, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB,
                                   sizeof(BCRYPT_CHAIN_MODE_ECB), 0);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Status = BCryptEncrypt(Key, (PUCHAR)AesPlain, 16, NULL, NULL, 0,
                               Buffer, sizeof(Buffer), &Result, 0);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(RtlEqualMemory(Buffer, AesEcbCipher, sizeof(AesEcbCipher)), "AES-ECB mismatch\n");

        Status = BCryptDestroyKey(Key);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }

    Status = BCryptCloseAlgorithmProvider(Alg, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);

    Status = BCryptOpenAlgorithmProvider(&Alg, L"NoSuchAlgorithm", NULL, 0);
    ok_eq_hex(Status, STATUS_NOT_FOUND);
    Status = BCryptOpenAlgorithmProvider(&Alg, BCRYPT_AES_ALGORITHM, L"Other Provider", 0);
    ok_eq_hex(Status, STATUS_NOT_FOUND);
}

static
VOID
TestRandom(VOID)
{
    UCHAR First[32], Second[32], Zero[32] = { 0 };
    BCRYPT_ALG_HANDLE Alg;
    NTSTATUS Status;

    RtlZeroMemory(First, sizeof(First));
    RtlZeroMemory(Second, sizeof(Second));

    Status = BCryptGenRandom(NULL, First, sizeof(First), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = BCryptGenRandom(BCRYPT_RNG_ALG_HANDLE, Second, sizeof(Second), 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(!RtlEqualMemory(First, Zero, sizeof(First)), "random output is all zeroes\n");
    ok(!RtlEqualMemory(First, Second, sizeof(First)), "random output repeats\n");

    Status = BCryptGenRandom(NULL, First, sizeof(First), 0);
    ok_eq_hex(Status, STATUS_INVALID_HANDLE);

    Status = BCryptOpenAlgorithmProvider(&Alg, BCRYPT_RNG_ALGORITHM, NULL, 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = BCryptGenRandom(Alg, First, sizeof(First), 0);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(!RtlEqualMemory(First, Second, sizeof(First)), "random output repeats\n");
        Status = BCryptCloseAlgorithmProvider(Alg, 0);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
}

/*
 * The calls ovpn-dco-win makes on its data path, under a spin lock: rekeying
 * (HKDF + key generation), encryption, decryption and key destruction.
 * Handles opened with BCRYPT_PROV_DISPATCH must work at DISPATCH_LEVEL.
 */
static
VOID
TestDispatchLevel(
    _In_ BCRYPT_ALG_HANDLE AesAlg,
    _In_ BCRYPT_ALG_HANDLE ChaChaAlg,
    _In_ BCRYPT_ALG_HANDLE HkdfAlg)
{
    BCRYPT_KEY_HANDLE HkdfKey, Key;
    BCryptBuffer Buffer;
    BCryptBufferDesc Desc;
    BCRYPT_ALG_HANDLE Alg;
    UCHAR Okm[42], Data[60], Tag[16], Random[16], Digest[32];
    ULONG Result;
    KIRQL OldIrql;
    NTSTATUS Status;

    KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);
    ok_irql(DISPATCH_LEVEL);

    /* Opening a provider is PASSIVE_LEVEL only */
    Status = BCryptOpenAlgorithmProvider(&Alg, BCRYPT_AES_ALGORITHM, NULL, BCRYPT_PROV_DISPATCH);
    ok_eq_hex(Status, STATUS_NOT_SUPPORTED);

    Status = BCryptGenerateSymmetricKey(HkdfAlg, &HkdfKey, NULL, 0, (PUCHAR)HkdfPrk,
                                        sizeof(HkdfPrk), 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        Status = BCryptSetProperty(HkdfKey, BCRYPT_HKDF_HASH_ALGORITHM,
                                   (PUCHAR)BCRYPT_SHA256_ALGORITHM,
                                   sizeof(BCRYPT_SHA256_ALGORITHM), 0);
        ok_eq_hex(Status, STATUS_SUCCESS);
        Status = BCryptSetProperty(HkdfKey, BCRYPT_HKDF_PRK_AND_FINALIZE, NULL, 0, 0);
        ok_eq_hex(Status, STATUS_SUCCESS);

        Buffer.cbBuffer = sizeof(HkdfInfo);
        Buffer.BufferType = KDF_HKDF_INFO;
        Buffer.pvBuffer = (PVOID)HkdfInfo;
        Desc.ulVersion = BCRYPTBUFFER_VERSION;
        Desc.cBuffers = 1;
        Desc.pBuffers = &Buffer;
        Status = BCryptKeyDerivation(HkdfKey, &Desc, Okm, sizeof(Okm), &Result, 0);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(RtlEqualMemory(Okm, HkdfOkm1, sizeof(Okm)), "HKDF at DISPATCH_LEVEL mismatch\n");
        Status = BCryptDestroyKey(HkdfKey);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }

    Status = BCryptGenerateSymmetricKey(AesAlg, &Key, NULL, 0, (PUCHAR)GcmKey, sizeof(GcmKey), 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    if (NT_SUCCESS(Status))
    {
        RtlCopyMemory(Data, GcmPlain, sizeof(Data));
        Status = AeadCrypt(Key, TRUE, GcmIv, GcmAad, sizeof(GcmAad), Data, Data, sizeof(Data),
                           Tag, sizeof(Tag), &Result);
        ok_eq_hex(Status, STATUS_SUCCESS);
        ok(RtlEqualMemory(Data, GcmCipher3, sizeof(Data)), "GCM at DISPATCH_LEVEL mismatch\n");
        ok(RtlEqualMemory(Tag, GcmTag4, sizeof(Tag)), "GCM tag at DISPATCH_LEVEL mismatch\n");

        Status = AeadCrypt(Key, FALSE, GcmIv, GcmAad, sizeof(GcmAad), Data, Data, sizeof(Data),
                           Tag, sizeof(Tag), &Result);
        ok_eq_hex(Status, STATUS_SUCCESS);

        Tag[0] ^= 0x01;
        Status = AeadCrypt(Key, FALSE, GcmIv, GcmAad, sizeof(GcmAad), Data, Data, sizeof(Data),
                           Tag, sizeof(Tag), &Result);
        ok_eq_hex(Status, STATUS_AUTH_TAG_MISMATCH);
        Status = BCryptDestroyKey(Key);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }

    if (ChaChaAlg)
    {
        Status = BCryptGenerateSymmetricKey(ChaChaAlg, &Key, NULL, 0, (PUCHAR)ChaChaKey,
                                            sizeof(ChaChaKey), 0);
        ok_eq_hex(Status, STATUS_SUCCESS);
        if (NT_SUCCESS(Status))
        {
            RtlCopyMemory(Data, ChaChaPlain, sizeof(Data));
            Status = AeadCrypt(Key, TRUE, ChaChaNonce, ChaChaAad, sizeof(ChaChaAad),
                               Data, Data, sizeof(Data), Tag, sizeof(Tag), &Result);
            ok_eq_hex(Status, STATUS_SUCCESS);
            ok(RtlEqualMemory(Data, ChaChaCipher, sizeof(Data)),
               "ChaCha20 at DISPATCH_LEVEL mismatch\n");
            Status = AeadCrypt(Key, FALSE, ChaChaNonce, ChaChaAad, sizeof(ChaChaAad),
                               Data, Data, sizeof(Data), Tag, sizeof(Tag), &Result);
            ok_eq_hex(Status, STATUS_SUCCESS);
            ok(RtlEqualMemory(Data, ChaChaPlain, sizeof(Data)),
               "ChaCha20 at DISPATCH_LEVEL round trip\n");
            Status = BCryptDestroyKey(Key);
            ok_eq_hex(Status, STATUS_SUCCESS);
        }
    }

    Status = BCryptGenRandom(NULL, Random, sizeof(Random), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    ok_eq_hex(Status, STATUS_SUCCESS);

    Status = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, NULL, 0, (PUCHAR)"abc", 3, Digest,
                        sizeof(Digest));
    ok_eq_hex(Status, STATUS_SUCCESS);
    ok(RtlEqualMemory(Digest, Sha256Abc, sizeof(Digest)), "SHA-256 at DISPATCH_LEVEL mismatch\n");

    ok_irql(DISPATCH_LEVEL);
    KeLowerIrql(OldIrql);
}

static
VOID
TestDispatch(VOID)
{
    BCRYPT_ALG_HANDLE AesAlg = NULL, ChaChaAlg = NULL, HkdfAlg = NULL;
    NTSTATUS Status;

    /* Opened at PASSIVE_LEVEL, used at DISPATCH_LEVEL */
    Status = BCryptOpenAlgorithmProvider(&AesAlg, BCRYPT_AES_ALGORITHM, NULL, BCRYPT_PROV_DISPATCH);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = BCryptSetProperty(AesAlg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM,
                               sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = BCryptOpenAlgorithmProvider(&HkdfAlg, BCRYPT_HKDF_ALGORITHM, NULL,
                                         BCRYPT_PROV_DISPATCH);
    ok_eq_hex(Status, STATUS_SUCCESS);
    Status = BCryptOpenAlgorithmProvider(&ChaChaAlg, BCRYPT_CHACHA20_POLY1305_ALGORITHM, NULL,
                                         BCRYPT_PROV_DISPATCH);
    ok_eq_hex(Status, STATUS_SUCCESS);

    if (AesAlg && HkdfAlg)
        TestDispatchLevel(AesAlg, ChaChaAlg, HkdfAlg);

    if (ChaChaAlg)
    {
        Status = BCryptCloseAlgorithmProvider(ChaChaAlg, 0);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
    if (HkdfAlg)
    {
        Status = BCryptCloseAlgorithmProvider(HkdfAlg, 0);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
    if (AesAlg)
    {
        Status = BCryptCloseAlgorithmProvider(AesAlg, 0);
        ok_eq_hex(Status, STATUS_SUCCESS);
    }
}

static
NTSTATUS
TestMessageHandler(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG ControlCode,
    _In_opt_ PVOID Buffer,
    _In_ SIZE_T InLength,
    _Inout_ PSIZE_T OutLength)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(InLength);
    UNREFERENCED_PARAMETER(OutLength);

    if (ControlCode != IOCTL_CNG_RUN_TESTS)
        return STATUS_NOT_SUPPORTED;

    ok_irql(PASSIVE_LEVEL);
    TestHash();
    TestHkdf();
    TestAesGcm();
    TestChaCha20Poly1305();
    TestAesCbcEcb();
    TestRandom();
    TestDispatch();
    return STATUS_SUCCESS;
}

NTSTATUS
TestEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PCUNICODE_STRING RegistryPath,
    _Out_ PCWSTR *DeviceName,
    _Inout_ INT *Flags)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);
    UNREFERENCED_PARAMETER(Flags);

    *DeviceName = L"Cng";
    return KmtRegisterMessageHandler(IOCTL_CNG_RUN_TESTS, NULL, TestMessageHandler);
}

VOID
TestUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(DriverObject);
}
