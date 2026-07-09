/*
 * PROJECT:     ReactOS Terminal Services
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Standard RDP security (RC4 + Server Proprietary Certificate)
 * COPYRIGHT:   Copyright 2026 ReactOS Terminal Services contributors
 */

#include <winsock2.h>
#include <windows.h>
#include <wincrypt.h>
#include "rdpcrypt.h"

#define NDEBUG
#include <debug.h>

/* MS-RDPBCGR security header flags. */
#define TERMSRV_SEC_EXCHANGE_PKT 0x0001
#define TERMSRV_SEC_ENCRYPT      0x0008
#define TERMSRV_SEC_INFO_PKT     0x0040

/* Fast-path input header flags (MS-RDPBCGR 2.2.8.1.2). */
#define TERMSRV_FASTPATH_INPUT_SECURE_CHECKSUM 0x40
#define TERMSRV_FASTPATH_INPUT_ENCRYPTED       0x80

/* Proprietary certificate constants (MS-RDPBCGR 2.2.1.4.3.1.1). */
#define TERMSRV_SIGNATURE_ALG_RSA    0x00000001
#define TERMSRV_KEY_EXCHANGE_ALG_RSA 0x00000001
#define TERMSRV_BB_RSA_KEY_BLOB      0x0006
#define TERMSRV_BB_RSA_SIGNATURE_BLOB 0x0008

/* ENCRYPTION_METHOD_128BIT / ENCRYPTION_LEVEL_LOW. */
#define TERMSRV_ENCRYPTION_METHOD_128BIT 0x00000002
#define TERMSRV_ENCRYPTION_LEVEL_LOW     0x00000001

/* -------------------------------------------------------------------------- */
/* Fixed keys                                                                  */
/* -------------------------------------------------------------------------- */

/*
 * Fixed 512-bit RSA server key (little-endian). The client encrypts its client
 * random with the advertised public key (modulus + exponent 65537); the server
 * decrypts with the private exponent below. A fixed key is standard practice
 * for RDP Standard Security servers (e.g. xrdp ships a generated-once key).
 */
static const UCHAR ServerRsaModulus[64] =
{
    0xb1, 0x2a, 0x21, 0x03, 0xc8, 0xfd, 0x7c, 0x3a, 0xcb, 0xee, 0x3f, 0xe5,
    0x07, 0x68, 0x18, 0xa3, 0xdd, 0x75, 0x55, 0xae, 0xc9, 0xfc, 0x24, 0x5f,
    0x80, 0xd9, 0x5d, 0x83, 0x9a, 0x3c, 0x67, 0x93, 0x35, 0x97, 0x49, 0x1d,
    0x62, 0xed, 0x38, 0x39, 0x36, 0x26, 0x48, 0x1a, 0xdf, 0x25, 0xdc, 0x6c,
    0x5f, 0x05, 0x91, 0x1c, 0x44, 0x51, 0x51, 0x41, 0x41, 0x29, 0xd7, 0x31,
    0x7c, 0x09, 0xd0, 0xac
};
static const UCHAR ServerRsaPrivateExponent[64] =
{
    0x81, 0x38, 0xc5, 0xe1, 0x88, 0x1c, 0x23, 0x33, 0xd1, 0x91, 0xa4, 0x17,
    0x07, 0x61, 0xe7, 0x1c, 0xcb, 0x22, 0xa9, 0x6d, 0x50, 0x4f, 0x59, 0xf5,
    0x1f, 0xcf, 0x04, 0x70, 0x3f, 0x5a, 0x71, 0xb1, 0xb3, 0x3a, 0xa4, 0x6b,
    0xb5, 0x54, 0x0b, 0x5b, 0x81, 0x7f, 0xbd, 0xe6, 0x41, 0x43, 0x57, 0xbd,
    0x0c, 0x16, 0x85, 0x82, 0x38, 0x28, 0x4e, 0xdf, 0xad, 0xdb, 0x8b, 0x69,
    0xd5, 0x2a, 0x49, 0x0b
};
/* public exponent 65537 (little-endian) */
static const UCHAR ServerRsaPublicExponent[4] = { 0x01, 0x00, 0x01, 0x00 };

/*
 * Well-known Terminal Services signing key (publicly documented). The
 * Proprietary Certificate's signature is produced with this private key; every
 * RDP client validates it against the matching hard-coded public key.
 * (Values from the FreeRDP project, little-endian.)
 */
static const UCHAR TsskModulus[64] =
{
    0x3d, 0x3a, 0x5e, 0xbd, 0x72, 0x43, 0x3e, 0xc9, 0x4d, 0xbb, 0xc1, 0x1e,
    0x4a, 0xba, 0x5f, 0xcb, 0x3e, 0x88, 0x20, 0x87, 0xef, 0xf5, 0xc1, 0xe2,
    0xd7, 0xb7, 0x6b, 0x9a, 0xf2, 0x52, 0x45, 0x95, 0xce, 0x63, 0x65, 0x6b,
    0x58, 0x3a, 0xfe, 0xef, 0x7c, 0xe7, 0xbf, 0xfe, 0x3d, 0xf6, 0x5c, 0x7d,
    0x6c, 0x5e, 0x06, 0x09, 0x1a, 0xf5, 0x61, 0xbb, 0x20, 0x93, 0x09, 0x5f,
    0x05, 0x6d, 0xea, 0x87
};
static const UCHAR TsskPrivateExponent[64] =
{
    0x87, 0xa7, 0x19, 0x32, 0xda, 0x11, 0x87, 0x55, 0x58, 0x00, 0x16, 0x16,
    0x25, 0x65, 0x68, 0xf8, 0x24, 0x3e, 0xe6, 0xfa, 0xe9, 0x67, 0x49, 0x94,
    0xcf, 0x92, 0xcc, 0x33, 0x99, 0xe8, 0x08, 0x60, 0x17, 0x9a, 0x12, 0x9f,
    0x24, 0xdd, 0xb1, 0x24, 0x99, 0xc7, 0x3a, 0xb8, 0x0a, 0x7b, 0x0d, 0xdd,
    0x35, 0x07, 0x79, 0x17, 0x0b, 0x51, 0x9b, 0xb3, 0xc7, 0x10, 0x01, 0x13,
    0xe7, 0x3f, 0xf3, 0x5f
};

/* -------------------------------------------------------------------------- */
/* MD5 (RFC 1321)                                                              */
/* -------------------------------------------------------------------------- */

typedef struct
{
    UINT32 State[4];
    UINT32 Count[2];
    UCHAR Buffer[64];
} TERMSRV_MD5_CTX;

#define MD5_F(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define MD5_G(x, y, z) (((x) & (z)) | ((y) & ~(z)))
#define MD5_H(x, y, z) ((x) ^ (y) ^ (z))
#define MD5_I(x, y, z) ((y) ^ ((x) | ~(z)))
#define MD5_ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define MD5_STEP(f, a, b, c, d, x, t, s) \
    do { (a) += f((b), (c), (d)) + (x) + (t); (a) = MD5_ROTL((a), (s)); (a) += (b); } while (0)

static VOID
Md5Transform(UINT32 State[4], const UCHAR Block[64])
{
    UINT32 a = State[0], b = State[1], c = State[2], d = State[3];
    UINT32 x[16];
    int i;

    for (i = 0; i < 16; i++)
        x[i] = (UINT32)Block[i * 4] | ((UINT32)Block[i * 4 + 1] << 8) |
               ((UINT32)Block[i * 4 + 2] << 16) | ((UINT32)Block[i * 4 + 3] << 24);

    MD5_STEP(MD5_F, a, b, c, d, x[0], 0xd76aa478, 7);
    MD5_STEP(MD5_F, d, a, b, c, x[1], 0xe8c7b756, 12);
    MD5_STEP(MD5_F, c, d, a, b, x[2], 0x242070db, 17);
    MD5_STEP(MD5_F, b, c, d, a, x[3], 0xc1bdceee, 22);
    MD5_STEP(MD5_F, a, b, c, d, x[4], 0xf57c0faf, 7);
    MD5_STEP(MD5_F, d, a, b, c, x[5], 0x4787c62a, 12);
    MD5_STEP(MD5_F, c, d, a, b, x[6], 0xa8304613, 17);
    MD5_STEP(MD5_F, b, c, d, a, x[7], 0xfd469501, 22);
    MD5_STEP(MD5_F, a, b, c, d, x[8], 0x698098d8, 7);
    MD5_STEP(MD5_F, d, a, b, c, x[9], 0x8b44f7af, 12);
    MD5_STEP(MD5_F, c, d, a, b, x[10], 0xffff5bb1, 17);
    MD5_STEP(MD5_F, b, c, d, a, x[11], 0x895cd7be, 22);
    MD5_STEP(MD5_F, a, b, c, d, x[12], 0x6b901122, 7);
    MD5_STEP(MD5_F, d, a, b, c, x[13], 0xfd987193, 12);
    MD5_STEP(MD5_F, c, d, a, b, x[14], 0xa679438e, 17);
    MD5_STEP(MD5_F, b, c, d, a, x[15], 0x49b40821, 22);

    MD5_STEP(MD5_G, a, b, c, d, x[1], 0xf61e2562, 5);
    MD5_STEP(MD5_G, d, a, b, c, x[6], 0xc040b340, 9);
    MD5_STEP(MD5_G, c, d, a, b, x[11], 0x265e5a51, 14);
    MD5_STEP(MD5_G, b, c, d, a, x[0], 0xe9b6c7aa, 20);
    MD5_STEP(MD5_G, a, b, c, d, x[5], 0xd62f105d, 5);
    MD5_STEP(MD5_G, d, a, b, c, x[10], 0x02441453, 9);
    MD5_STEP(MD5_G, c, d, a, b, x[15], 0xd8a1e681, 14);
    MD5_STEP(MD5_G, b, c, d, a, x[4], 0xe7d3fbc8, 20);
    MD5_STEP(MD5_G, a, b, c, d, x[9], 0x21e1cde6, 5);
    MD5_STEP(MD5_G, d, a, b, c, x[14], 0xc33707d6, 9);
    MD5_STEP(MD5_G, c, d, a, b, x[3], 0xf4d50d87, 14);
    MD5_STEP(MD5_G, b, c, d, a, x[8], 0x455a14ed, 20);
    MD5_STEP(MD5_G, a, b, c, d, x[13], 0xa9e3e905, 5);
    MD5_STEP(MD5_G, d, a, b, c, x[2], 0xfcefa3f8, 9);
    MD5_STEP(MD5_G, c, d, a, b, x[7], 0x676f02d9, 14);
    MD5_STEP(MD5_G, b, c, d, a, x[12], 0x8d2a4c8a, 20);

    MD5_STEP(MD5_H, a, b, c, d, x[5], 0xfffa3942, 4);
    MD5_STEP(MD5_H, d, a, b, c, x[8], 0x8771f681, 11);
    MD5_STEP(MD5_H, c, d, a, b, x[11], 0x6d9d6122, 16);
    MD5_STEP(MD5_H, b, c, d, a, x[14], 0xfde5380c, 23);
    MD5_STEP(MD5_H, a, b, c, d, x[1], 0xa4beea44, 4);
    MD5_STEP(MD5_H, d, a, b, c, x[4], 0x4bdecfa9, 11);
    MD5_STEP(MD5_H, c, d, a, b, x[7], 0xf6bb4b60, 16);
    MD5_STEP(MD5_H, b, c, d, a, x[10], 0xbebfbc70, 23);
    MD5_STEP(MD5_H, a, b, c, d, x[13], 0x289b7ec6, 4);
    MD5_STEP(MD5_H, d, a, b, c, x[0], 0xeaa127fa, 11);
    MD5_STEP(MD5_H, c, d, a, b, x[3], 0xd4ef3085, 16);
    MD5_STEP(MD5_H, b, c, d, a, x[6], 0x04881d05, 23);
    MD5_STEP(MD5_H, a, b, c, d, x[9], 0xd9d4d039, 4);
    MD5_STEP(MD5_H, d, a, b, c, x[12], 0xe6db99e5, 11);
    MD5_STEP(MD5_H, c, d, a, b, x[15], 0x1fa27cf8, 16);
    MD5_STEP(MD5_H, b, c, d, a, x[2], 0xc4ac5665, 23);

    MD5_STEP(MD5_I, a, b, c, d, x[0], 0xf4292244, 6);
    MD5_STEP(MD5_I, d, a, b, c, x[7], 0x432aff97, 10);
    MD5_STEP(MD5_I, c, d, a, b, x[14], 0xab9423a7, 15);
    MD5_STEP(MD5_I, b, c, d, a, x[5], 0xfc93a039, 21);
    MD5_STEP(MD5_I, a, b, c, d, x[12], 0x655b59c3, 6);
    MD5_STEP(MD5_I, d, a, b, c, x[3], 0x8f0ccc92, 10);
    MD5_STEP(MD5_I, c, d, a, b, x[10], 0xffeff47d, 15);
    MD5_STEP(MD5_I, b, c, d, a, x[1], 0x85845dd1, 21);
    MD5_STEP(MD5_I, a, b, c, d, x[8], 0x6fa87e4f, 6);
    MD5_STEP(MD5_I, d, a, b, c, x[15], 0xfe2ce6e0, 10);
    MD5_STEP(MD5_I, c, d, a, b, x[6], 0xa3014314, 15);
    MD5_STEP(MD5_I, b, c, d, a, x[13], 0x4e0811a1, 21);
    MD5_STEP(MD5_I, a, b, c, d, x[4], 0xf7537e82, 6);
    MD5_STEP(MD5_I, d, a, b, c, x[11], 0xbd3af235, 10);
    MD5_STEP(MD5_I, c, d, a, b, x[2], 0x2ad7d2bb, 15);
    MD5_STEP(MD5_I, b, c, d, a, x[9], 0xeb86d391, 21);

    State[0] += a;
    State[1] += b;
    State[2] += c;
    State[3] += d;
}

static VOID
Md5Init(TERMSRV_MD5_CTX *Ctx)
{
    Ctx->State[0] = 0x67452301;
    Ctx->State[1] = 0xefcdab89;
    Ctx->State[2] = 0x98badcfe;
    Ctx->State[3] = 0x10325476;
    Ctx->Count[0] = 0;
    Ctx->Count[1] = 0;
}

static VOID
Md5Update(TERMSRV_MD5_CTX *Ctx, const UCHAR *Data, SIZE_T Length)
{
    SIZE_T Index = (Ctx->Count[0] >> 3) & 0x3f;
    SIZE_T PartLen = 64 - Index;
    SIZE_T i;

    Ctx->Count[0] += (UINT32)(Length << 3);
    if (Ctx->Count[0] < (Length << 3))
        Ctx->Count[1]++;
    Ctx->Count[1] += (UINT32)(Length >> 29);

    if (Length >= PartLen)
    {
        CopyMemory(&Ctx->Buffer[Index], Data, PartLen);
        Md5Transform(Ctx->State, Ctx->Buffer);
        for (i = PartLen; i + 63 < Length; i += 64)
            Md5Transform(Ctx->State, &Data[i]);
        Index = 0;
    }
    else
    {
        i = 0;
    }

    CopyMemory(&Ctx->Buffer[Index], &Data[i], Length - i);
}

static VOID
Md5Final(TERMSRV_MD5_CTX *Ctx, UCHAR Digest[16])
{
    UCHAR Bits[8];
    UCHAR Pad[64];
    SIZE_T Index, PadLen;
    int i;

    for (i = 0; i < 4; i++)
    {
        Bits[i] = (UCHAR)(Ctx->Count[0] >> (i * 8));
        Bits[i + 4] = (UCHAR)(Ctx->Count[1] >> (i * 8));
    }

    ZeroMemory(Pad, sizeof(Pad));
    Pad[0] = 0x80;

    Index = (Ctx->Count[0] >> 3) & 0x3f;
    PadLen = (Index < 56) ? (56 - Index) : (120 - Index);
    Md5Update(Ctx, Pad, PadLen);
    Md5Update(Ctx, Bits, 8);

    for (i = 0; i < 4; i++)
    {
        Digest[i] = (UCHAR)(Ctx->State[0] >> (i * 8));
        Digest[i + 4] = (UCHAR)(Ctx->State[1] >> (i * 8));
        Digest[i + 8] = (UCHAR)(Ctx->State[2] >> (i * 8));
        Digest[i + 12] = (UCHAR)(Ctx->State[3] >> (i * 8));
    }
}

/* -------------------------------------------------------------------------- */
/* SHA-1 (RFC 3174)                                                            */
/* -------------------------------------------------------------------------- */

typedef struct
{
    UINT32 State[5];
    UINT32 Count[2];
    UCHAR Buffer[64];
} TERMSRV_SHA1_CTX;

#define SHA1_ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static VOID
Sha1Transform(UINT32 State[5], const UCHAR Block[64])
{
    UINT32 w[80];
    UINT32 a, b, c, d, e, t;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((UINT32)Block[i * 4] << 24) | ((UINT32)Block[i * 4 + 1] << 16) |
               ((UINT32)Block[i * 4 + 2] << 8) | (UINT32)Block[i * 4 + 3];
    for (i = 16; i < 80; i++)
        w[i] = SHA1_ROTL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    a = State[0]; b = State[1]; c = State[2]; d = State[3]; e = State[4];

    for (i = 0; i < 80; i++)
    {
        UINT32 f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
        else { f = b ^ c ^ d; k = 0xca62c1d6; }
        t = SHA1_ROTL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = SHA1_ROTL(b, 30); b = a; a = t;
    }

    State[0] += a; State[1] += b; State[2] += c; State[3] += d; State[4] += e;
}

static VOID
Sha1Init(TERMSRV_SHA1_CTX *Ctx)
{
    Ctx->State[0] = 0x67452301;
    Ctx->State[1] = 0xefcdab89;
    Ctx->State[2] = 0x98badcfe;
    Ctx->State[3] = 0x10325476;
    Ctx->State[4] = 0xc3d2e1f0;
    Ctx->Count[0] = 0;
    Ctx->Count[1] = 0;
}

static VOID
Sha1Update(TERMSRV_SHA1_CTX *Ctx, const UCHAR *Data, SIZE_T Length)
{
    SIZE_T Index = (Ctx->Count[0] >> 3) & 0x3f;
    SIZE_T PartLen = 64 - Index;
    SIZE_T i;

    Ctx->Count[0] += (UINT32)(Length << 3);
    if (Ctx->Count[0] < (Length << 3))
        Ctx->Count[1]++;
    Ctx->Count[1] += (UINT32)(Length >> 29);

    if (Length >= PartLen)
    {
        CopyMemory(&Ctx->Buffer[Index], Data, PartLen);
        Sha1Transform(Ctx->State, Ctx->Buffer);
        for (i = PartLen; i + 63 < Length; i += 64)
            Sha1Transform(Ctx->State, &Data[i]);
        Index = 0;
    }
    else
    {
        i = 0;
    }

    CopyMemory(&Ctx->Buffer[Index], &Data[i], Length - i);
}

static VOID
Sha1Final(TERMSRV_SHA1_CTX *Ctx, UCHAR Digest[20])
{
    UCHAR Bits[8];
    UCHAR Pad[64];
    SIZE_T Index, PadLen;
    int i;

    for (i = 0; i < 4; i++)
    {
        Bits[i] = (UCHAR)(Ctx->Count[1] >> ((3 - i) * 8));
        Bits[i + 4] = (UCHAR)(Ctx->Count[0] >> ((3 - i) * 8));
    }

    ZeroMemory(Pad, sizeof(Pad));
    Pad[0] = 0x80;

    Index = (Ctx->Count[0] >> 3) & 0x3f;
    PadLen = (Index < 56) ? (56 - Index) : (120 - Index);
    Sha1Update(Ctx, Pad, PadLen);
    Sha1Update(Ctx, Bits, 8);

    for (i = 0; i < 20; i++)
        Digest[i] = (UCHAR)(Ctx->State[i / 4] >> ((3 - (i & 3)) * 8));
}

/* -------------------------------------------------------------------------- */
/* RC4                                                                         */
/* -------------------------------------------------------------------------- */

static VOID
Rc4Init(TERMSRV_RC4_STATE *State, const UCHAR *Key, SIZE_T KeyLength)
{
    int i, j = 0;

    for (i = 0; i < 256; i++)
        State->S[i] = (UCHAR)i;

    for (i = 0; i < 256; i++)
    {
        UCHAR t;
        j = (j + State->S[i] + Key[i % KeyLength]) & 0xff;
        t = State->S[i];
        State->S[i] = State->S[j];
        State->S[j] = t;
    }

    State->I = 0;
    State->J = 0;
}

static VOID
Rc4Crypt(TERMSRV_RC4_STATE *State, UCHAR *Data, SIZE_T Length)
{
    SIZE_T k;

    for (k = 0; k < Length; k++)
    {
        UCHAR t;
        State->I = (UCHAR)(State->I + 1);
        State->J = (UCHAR)(State->J + State->S[State->I]);
        t = State->S[State->I];
        State->S[State->I] = State->S[State->J];
        State->S[State->J] = t;
        Data[k] ^= State->S[(UCHAR)(State->S[State->I] + State->S[State->J])];
    }
}

/* -------------------------------------------------------------------------- */
/* Fixed-width big-integer modular exponentiation (little-endian)              */
/* -------------------------------------------------------------------------- */

#define BN_LIMBS 40 /* 1280 bits: ample headroom for 512-bit RSA */

typedef struct
{
    UINT32 v[BN_LIMBS];
} TERMSRV_BN;

static VOID
BnZero(TERMSRV_BN *a)
{
    ZeroMemory(a, sizeof(*a));
}

static VOID
BnFromLe(TERMSRV_BN *a, const UCHAR *b, SIZE_T n)
{
    SIZE_T i;
    BnZero(a);
    for (i = 0; i < n && (i >> 2) < BN_LIMBS; i++)
        a->v[i >> 2] |= (UINT32)b[i] << (8 * (i & 3));
}

static VOID
BnToLe(const TERMSRV_BN *a, UCHAR *b, SIZE_T n)
{
    SIZE_T i;
    for (i = 0; i < n; i++)
        b[i] = ((i >> 2) < BN_LIMBS) ? (UCHAR)(a->v[i >> 2] >> (8 * (i & 3))) : 0;
}

static int
BnBit(const TERMSRV_BN *a, int i)
{
    return (int)((a->v[i >> 5] >> (i & 31)) & 1);
}

static int
BnCmp(const TERMSRV_BN *a, const TERMSRV_BN *b)
{
    int i;
    for (i = BN_LIMBS - 1; i >= 0; i--)
    {
        if (a->v[i] != b->v[i])
            return (a->v[i] < b->v[i]) ? -1 : 1;
    }
    return 0;
}

/* a -= b (requires a >= b) */
static VOID
BnSub(TERMSRV_BN *a, const TERMSRV_BN *b)
{
    UINT64 borrow = 0;
    int i;
    for (i = 0; i < BN_LIMBS; i++)
    {
        UINT64 t = (UINT64)a->v[i] - b->v[i] - borrow;
        a->v[i] = (UINT32)t;
        borrow = (t >> 63) & 1;
    }
}

/* a += b */
static VOID
BnAdd(TERMSRV_BN *a, const TERMSRV_BN *b)
{
    UINT64 carry = 0;
    int i;
    for (i = 0; i < BN_LIMBS; i++)
    {
        UINT64 t = (UINT64)a->v[i] + b->v[i] + carry;
        a->v[i] = (UINT32)t;
        carry = t >> 32;
    }
}

/* a <<= 1 */
static VOID
BnShl1(TERMSRV_BN *a)
{
    UINT32 carry = 0;
    int i;
    for (i = 0; i < BN_LIMBS; i++)
    {
        UINT32 next = a->v[i] >> 31;
        a->v[i] = (a->v[i] << 1) | carry;
        carry = next;
    }
}

/* a %= m (reduce; a assumed < m*2^k for small k, uses shift-subtract) */
static VOID
BnMod(TERMSRV_BN *a, const TERMSRV_BN *m)
{
    while (BnCmp(a, m) >= 0)
        BnSub(a, m);
}

/* r = (a + b) mod m, with a,b < m */
static VOID
BnAddMod(TERMSRV_BN *r, const TERMSRV_BN *a, const TERMSRV_BN *b, const TERMSRV_BN *m)
{
    *r = *a;
    BnAdd(r, b);
    if (BnCmp(r, m) >= 0)
        BnSub(r, m);
}

/* r = (a * b) mod m, with a,b < m (double-and-add, keeps operands < m) */
static VOID
BnMulMod(TERMSRV_BN *r, const TERMSRV_BN *a, const TERMSRV_BN *b, const TERMSRV_BN *m)
{
    TERMSRV_BN result;
    TERMSRV_BN addend;
    int i;

    BnZero(&result);
    addend = *a;

    for (i = 0; i < BN_LIMBS * 32; i++)
    {
        if (BnBit(b, i))
            BnAddMod(&result, &result, &addend, m);

        /* addend = (addend * 2) mod m */
        BnShl1(&addend);
        if (BnCmp(&addend, m) >= 0)
            BnSub(&addend, m);
    }

    *r = result;
}

/* out = (base ^ exp) mod m, all little-endian byte arrays; out is modLen bytes */
static VOID
BnModExp(UCHAR *out, SIZE_T modLen,
         const UCHAR *base, SIZE_T baseLen,
         const UCHAR *exp, SIZE_T expLen,
         const UCHAR *mod)
{
    TERMSRV_BN b, e, m, result;
    int i, topBit;

    BnFromLe(&m, mod, modLen);
    BnFromLe(&b, base, baseLen);
    BnFromLe(&e, exp, expLen);
    BnMod(&b, &m);

    BnZero(&result);
    result.v[0] = 1;

    topBit = BN_LIMBS * 32 - 1;
    while (topBit > 0 && !BnBit(&e, topBit))
        topBit--;

    for (i = topBit; i >= 0; i--)
    {
        BnMulMod(&result, &result, &result, &m);
        if (BnBit(&e, i))
            BnMulMod(&result, &result, &b, &m);
    }

    BnToLe(&result, out, modLen);
}

/* -------------------------------------------------------------------------- */
/* Certificate + key derivation helpers                                        */
/* -------------------------------------------------------------------------- */

static VOID
WriteLe16(UCHAR *b, USHORT v)
{
    b[0] = (UCHAR)v;
    b[1] = (UCHAR)(v >> 8);
}

static VOID
WriteLe32(UCHAR *b, ULONG v)
{
    b[0] = (UCHAR)v;
    b[1] = (UCHAR)(v >> 8);
    b[2] = (UCHAR)(v >> 16);
    b[3] = (UCHAR)(v >> 24);
}

static ULONG
ReadLe32(const UCHAR *b)
{
    return (ULONG)b[0] | ((ULONG)b[1] << 8) | ((ULONG)b[2] << 16) | ((ULONG)b[3] << 24);
}

/*
 * Append the Server Proprietary Certificate (PROPRIETARYSERVERCERTIFICATE
 * preceded by dwVersion) to Buffer. Returns bytes written or 0 on overflow.
 */
static SIZE_T
BuildProprietaryCertificate(UCHAR *Buffer, SIZE_T BufferLength)
{
    const ULONG ModulusLength = sizeof(ServerRsaModulus); /* 64 */
    const ULONG KeyLen = ModulusLength + 8;
    const ULONG BitLen = ModulusLength * 8;
    const ULONG DataLen = (BitLen / 8) - 1;
    const USHORT PublicKeyBlobLen = (USHORT)(16 + sizeof(ServerRsaPublicExponent) + KeyLen);
    SIZE_T Pos = 0;
    SIZE_T SigDataStart;
    UCHAR SignatureBlock[64];
    UCHAR EncryptedSignature[64];
    TERMSRV_MD5_CTX Md5;
    int i;

    /* dwVersion + dwSigAlgId + dwKeyAlgId + wPublicKeyBlobType + wPublicKeyBlobLen
     * + RSA_PUBLIC_KEY blob + wSignatureBlobType + wSignatureBlobLen + sig(64) + pad(8) */
    if (BufferLength < (SIZE_T)(4 + 10 + 2 + PublicKeyBlobLen + 4 + 64 + 8))
        return 0;

    WriteLe32(&Buffer[Pos], 0x00000001); Pos += 4; /* dwVersion = CERT_CHAIN_VERSION_1 */
    SigDataStart = Pos - 4;                         /* signature covers dwVersion onward */

    WriteLe32(&Buffer[Pos], TERMSRV_SIGNATURE_ALG_RSA); Pos += 4;
    WriteLe32(&Buffer[Pos], TERMSRV_KEY_EXCHANGE_ALG_RSA); Pos += 4;
    WriteLe16(&Buffer[Pos], TERMSRV_BB_RSA_KEY_BLOB); Pos += 2;
    WriteLe16(&Buffer[Pos], PublicKeyBlobLen); Pos += 2;

    /* RSA_PUBLIC_KEY */
    Buffer[Pos++] = 'R'; Buffer[Pos++] = 'S'; Buffer[Pos++] = 'A'; Buffer[Pos++] = '1';
    WriteLe32(&Buffer[Pos], KeyLen); Pos += 4;
    WriteLe32(&Buffer[Pos], BitLen); Pos += 4;
    WriteLe32(&Buffer[Pos], DataLen); Pos += 4;
    CopyMemory(&Buffer[Pos], ServerRsaPublicExponent, sizeof(ServerRsaPublicExponent));
    Pos += sizeof(ServerRsaPublicExponent);
    CopyMemory(&Buffer[Pos], ServerRsaModulus, ModulusLength);
    Pos += ModulusLength;
    ZeroMemory(&Buffer[Pos], 8); Pos += 8; /* modulus zero padding */

    /* Signature: MD5 hash of everything written so far (from dwVersion), placed
     * into the low 16 bytes of the fixed 0xFF padding block, then RSA "signed"
     * (raw private-key modexp) with the Terminal Services signing key. */
    SignatureBlock[0] = 0; /* placeholder, overwritten by MD5 below */
    for (i = 16; i < 64; i++)
        SignatureBlock[i] = 0xFF;
    SignatureBlock[16] = 0x00;
    SignatureBlock[63] = 0x01;

    Md5Init(&Md5);
    Md5Update(&Md5, &Buffer[SigDataStart], Pos - SigDataStart);
    Md5Final(&Md5, SignatureBlock); /* writes 16 bytes into [0..15] */

    BnModExp(EncryptedSignature, sizeof(EncryptedSignature),
             SignatureBlock, sizeof(SignatureBlock),
             TsskPrivateExponent, sizeof(TsskPrivateExponent),
             TsskModulus);

    WriteLe16(&Buffer[Pos], TERMSRV_BB_RSA_SIGNATURE_BLOB); Pos += 2;
    WriteLe16(&Buffer[Pos], (USHORT)(sizeof(EncryptedSignature) + 8)); Pos += 2;
    CopyMemory(&Buffer[Pos], EncryptedSignature, sizeof(EncryptedSignature));
    Pos += sizeof(EncryptedSignature);
    ZeroMemory(&Buffer[Pos], 8); Pos += 8; /* signature zero padding */

    return Pos;
}

/* SaltedHash(S, I) = MD5(S + SHA1(I + S + ClientRandom + ServerRandom)) */
static VOID
SaltedHash(const UCHAR *Secret, SIZE_T SecretLength,
           const UCHAR *Salt, SIZE_T SaltLength,
           const UCHAR *ClientRandom, const UCHAR *ServerRandom,
           UCHAR Output[16])
{
    TERMSRV_SHA1_CTX Sha1;
    TERMSRV_MD5_CTX Md5;
    UCHAR ShaDigest[20];

    Sha1Init(&Sha1);
    Sha1Update(&Sha1, Salt, SaltLength);
    Sha1Update(&Sha1, Secret, SecretLength);
    Sha1Update(&Sha1, ClientRandom, TERMSRV_RDP_SERVER_RANDOM_LENGTH);
    Sha1Update(&Sha1, ServerRandom, TERMSRV_RDP_SERVER_RANDOM_LENGTH);
    Sha1Final(&Sha1, ShaDigest);

    Md5Init(&Md5);
    Md5Update(&Md5, Secret, SecretLength);
    Md5Update(&Md5, ShaDigest, sizeof(ShaDigest));
    Md5Final(&Md5, Output);
}

/* FinalHash(K) = MD5(K + ClientRandom + ServerRandom) */
static VOID
FinalHash(const UCHAR *Key, const UCHAR *ClientRandom, const UCHAR *ServerRandom, UCHAR Output[16])
{
    TERMSRV_MD5_CTX Md5;
    Md5Init(&Md5);
    Md5Update(&Md5, Key, TERMSRV_RDP_SESSION_KEY_LENGTH);
    Md5Update(&Md5, ClientRandom, TERMSRV_RDP_SERVER_RANDOM_LENGTH);
    Md5Update(&Md5, ServerRandom, TERMSRV_RDP_SERVER_RANDOM_LENGTH);
    Md5Final(&Md5, Output);
}

static VOID
DeriveSessionKeys(TERMSRV_RDP_CRYPT *Crypt)
{
    UCHAR PreMasterSecret[48];
    UCHAR MasterSecret[48];
    UCHAR SessionKeyBlob[48];
    static const UCHAR SaltA[] = { 0x41 };
    static const UCHAR SaltBB[] = { 0x42, 0x42 };
    static const UCHAR SaltCCC[] = { 0x43, 0x43, 0x43 };
    static const UCHAR SaltX[] = { 0x58 };
    static const UCHAR SaltYY[] = { 0x59, 0x59 };
    static const UCHAR SaltZZZ[] = { 0x5A, 0x5A, 0x5A };
    const UCHAR *Cr = Crypt->ClientRandom;
    const UCHAR *Sr = Crypt->ServerRandom;

    /* PreMasterSecret = first 192 bits of ClientRandom + first 192 bits of ServerRandom */
    CopyMemory(&PreMasterSecret[0], Cr, 24);
    CopyMemory(&PreMasterSecret[24], Sr, 24);

    SaltedHash(PreMasterSecret, 48, SaltA, sizeof(SaltA), Cr, Sr, &MasterSecret[0]);
    SaltedHash(PreMasterSecret, 48, SaltBB, sizeof(SaltBB), Cr, Sr, &MasterSecret[16]);
    SaltedHash(PreMasterSecret, 48, SaltCCC, sizeof(SaltCCC), Cr, Sr, &MasterSecret[32]);

    SaltedHash(MasterSecret, 48, SaltX, sizeof(SaltX), Cr, Sr, &SessionKeyBlob[0]);
    SaltedHash(MasterSecret, 48, SaltYY, sizeof(SaltYY), Cr, Sr, &SessionKeyBlob[16]);
    SaltedHash(MasterSecret, 48, SaltZZZ, sizeof(SaltZZZ), Cr, Sr, &SessionKeyBlob[32]);

    /* MACKey = first 128 bits of SessionKeyBlob */
    CopyMemory(Crypt->MacKey, &SessionKeyBlob[0], 16);

    /* Server decrypt key == client encrypt key == FinalHash(third 128 bits). */
    FinalHash(&SessionKeyBlob[32], Cr, Sr, Crypt->InitialDecryptKey);
    CopyMemory(Crypt->CurrentDecryptKey, Crypt->InitialDecryptKey, 16);

    Rc4Init(&Crypt->Rc4Decrypt, Crypt->CurrentDecryptKey, 16);
    Crypt->DecryptPacketCount = 0;
}

/* Regenerate the client->server RC4 key after 4096 packets (MS-RDPBCGR 5.3.7). */
static VOID
UpdateDecryptKey(TERMSRV_RDP_CRYPT *Crypt)
{
    TERMSRV_SHA1_CTX Sha1;
    TERMSRV_MD5_CTX Md5;
    TERMSRV_RC4_STATE Temp;
    UCHAR Pad1[40];
    UCHAR Pad2[48];
    UCHAR ShaDigest[20];
    UCHAR TempKey[16];

    FillMemory(Pad1, sizeof(Pad1), 0x36);
    FillMemory(Pad2, sizeof(Pad2), 0x5C);

    Sha1Init(&Sha1);
    Sha1Update(&Sha1, Crypt->InitialDecryptKey, 16);
    Sha1Update(&Sha1, Pad1, sizeof(Pad1));
    Sha1Update(&Sha1, Crypt->CurrentDecryptKey, 16);
    Sha1Final(&Sha1, ShaDigest);

    Md5Init(&Md5);
    Md5Update(&Md5, Crypt->InitialDecryptKey, 16);
    Md5Update(&Md5, Pad2, sizeof(Pad2));
    Md5Update(&Md5, ShaDigest, sizeof(ShaDigest));
    Md5Final(&Md5, TempKey);

    Rc4Init(&Temp, TempKey, 16);
    Rc4Crypt(&Temp, TempKey, 16);

    CopyMemory(Crypt->CurrentDecryptKey, TempKey, 16);
    Rc4Init(&Crypt->Rc4Decrypt, Crypt->CurrentDecryptKey, 16);
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                  */
/* -------------------------------------------------------------------------- */

VOID
TermSrvCryptInit(
    _Out_ TERMSRV_RDP_CRYPT *Crypt)
{
    HCRYPTPROV Provider = 0;

    ZeroMemory(Crypt, sizeof(*Crypt));

    if (CryptAcquireContextW(&Provider, NULL, NULL, PROV_RSA_FULL,
                             CRYPT_VERIFYCONTEXT | CRYPT_SILENT) &&
        CryptGenRandom(Provider, sizeof(Crypt->ServerRandom), Crypt->ServerRandom))
    {
        CryptReleaseContext(Provider, 0);
        return;
    }

    if (Provider)
        CryptReleaseContext(Provider, 0);

    /* Fallback PRNG if the crypto provider is unavailable: the server random
     * only needs to be unpredictable-ish and unique per connection. */
    {
        ULONG seed = GetTickCount() ^ (GetCurrentThreadId() << 8) ^ GetCurrentProcessId();
        LARGE_INTEGER perf;
        SIZE_T i;
        if (QueryPerformanceCounter(&perf))
            seed ^= perf.LowPart ^ perf.HighPart;
        for (i = 0; i < sizeof(Crypt->ServerRandom); i++)
        {
            seed = seed * 1103515245 + 12345;
            Crypt->ServerRandom[i] = (UCHAR)(seed >> 16);
        }
    }
}

SIZE_T
TermSrvCryptBuildServerSecurityBlock(
    _Out_writes_bytes_to_(BufferLength, return) UCHAR *Buffer,
    _In_ SIZE_T BufferLength,
    _In_ const TERMSRV_RDP_CRYPT *Crypt)
{
    SIZE_T Pos;
    SIZE_T CertLength;
    UCHAR CertBuffer[256];
    USHORT BlockLength;

    CertLength = BuildProprietaryCertificate(CertBuffer, sizeof(CertBuffer));
    if (CertLength == 0)
        return 0;

    /* header(4) + method(4) + level(4) + randomLen(4) + certLen(4) + random(32) + cert */
    BlockLength = (USHORT)(4 + 4 + 4 + 4 + 4 + TERMSRV_RDP_SERVER_RANDOM_LENGTH + CertLength);
    if (Buffer == NULL || BufferLength < BlockLength)
        return 0;

    Pos = 0;
    WriteLe16(&Buffer[Pos], 0x0c02); Pos += 2;              /* type = SC_SECURITY */
    WriteLe16(&Buffer[Pos], BlockLength); Pos += 2;         /* length (incl. header) */
    WriteLe32(&Buffer[Pos], TERMSRV_ENCRYPTION_METHOD_128BIT); Pos += 4;
    WriteLe32(&Buffer[Pos], TERMSRV_ENCRYPTION_LEVEL_LOW); Pos += 4;
    WriteLe32(&Buffer[Pos], TERMSRV_RDP_SERVER_RANDOM_LENGTH); Pos += 4;
    WriteLe32(&Buffer[Pos], (ULONG)CertLength); Pos += 4;
    CopyMemory(&Buffer[Pos], Crypt->ServerRandom, TERMSRV_RDP_SERVER_RANDOM_LENGTH);
    Pos += TERMSRV_RDP_SERVER_RANDOM_LENGTH;
    CopyMemory(&Buffer[Pos], CertBuffer, CertLength);
    Pos += CertLength;

    return Pos;
}

BOOL
TermSrvCryptCompleteKeyExchange(
    _Inout_ TERMSRV_RDP_CRYPT *Crypt,
    _In_reads_bytes_(PayloadLength) const UCHAR *Payload,
    _In_ SIZE_T PayloadLength)
{
    ULONG EncryptedLength;
    UCHAR Decrypted[64];

    if (Crypt->Enabled)
        return TRUE;

    /* Payload = length determinant (4 bytes) + encrypted client random. */
    if (PayloadLength < 4)
        return FALSE;

    EncryptedLength = ReadLe32(Payload);
    if (EncryptedLength < sizeof(ServerRsaModulus) ||
        (SIZE_T)EncryptedLength + 4 > PayloadLength)
    {
        DPRINT1("termsrv: bad security-exchange length %lu (payload %lu)\n",
                EncryptedLength, (ULONG)PayloadLength);
        return FALSE;
    }

    /* Raw RSA private-exponent decrypt: m = c^d mod n (little-endian). Only the
     * first ModulusLength bytes of the ciphertext are significant. */
    BnModExp(Decrypted, sizeof(Decrypted),
             &Payload[4], sizeof(ServerRsaModulus),
             ServerRsaPrivateExponent, sizeof(ServerRsaPrivateExponent),
             ServerRsaModulus);

    CopyMemory(Crypt->ClientRandom, Decrypted, TERMSRV_RDP_SERVER_RANDOM_LENGTH);

    DeriveSessionKeys(Crypt);
    Crypt->Enabled = TRUE;
    DPRINT("termsrv: RDP standard security enabled (128-bit RC4)\n");
    return TRUE;
}

/*
 * RC4-decrypt a client->server payload of PayloadLength bytes in place and
 * advance the packet counter, re-keying every 4096 packets.
 */
static VOID
DecryptStream(TERMSRV_RDP_CRYPT *Crypt, UCHAR *Payload, SIZE_T PayloadLength)
{
    if (Crypt->DecryptPacketCount != 0 && (Crypt->DecryptPacketCount % 4096) == 0)
        UpdateDecryptKey(Crypt);
    Crypt->DecryptPacketCount++;
    Rc4Crypt(&Crypt->Rc4Decrypt, Payload, PayloadLength);
}

/* Slow-path TPKT/MCS Send Data: strip MAC + RC4-decrypt when SEC_ENCRYPT set. */
static BOOL
UnwrapSlowPath(TERMSRV_RDP_CRYPT *Crypt, UCHAR *Buffer, SIZE_T *Length)
{
    SIZE_T Total = *Length;
    SIZE_T McsHeader;   /* offset of the MCS user-data length determinant */
    SIZE_T PerBytes;
    SIZE_T PayloadOffset;
    SIZE_T PayloadLength;
    SIZE_T PlainLength;
    ULONG Flags;

    /* TPKT(4) + X.224(3) then MCS Send-Data-Request 0x64. */
    if (Total < 7 + 7 || Buffer[7] != 0x64)
        return TRUE; /* not an MCS data request; nothing to do */

    McsHeader = 7 + 6; /* 0x64, initiator(2), channelId(2), priority(1) */
    if (Buffer[McsHeader] & 0x80)
    {
        PerBytes = 2;
        PayloadLength = ((SIZE_T)(Buffer[McsHeader] & 0x7f) << 8) | Buffer[McsHeader + 1];
    }
    else
    {
        PerBytes = 1;
        PayloadLength = Buffer[McsHeader];
    }

    PayloadOffset = McsHeader + PerBytes;
    if (PayloadOffset + PayloadLength > Total || PayloadLength < 4)
        return TRUE;

    Flags = ReadLe32(&Buffer[PayloadOffset]);
    if (!(Flags & TERMSRV_SEC_ENCRYPT))
        return TRUE; /* security exchange / unencrypted control */

    /* Layout after the 4-byte security flags: MAC(8) + ciphertext. */
    if (PayloadLength < 4 + TERMSRV_RDP_MAC_LENGTH)
        return TRUE;

    {
        SIZE_T StripLength = 4 + TERMSRV_RDP_MAC_LENGTH; /* security flags + MAC */
        UCHAR *Cipher = &Buffer[PayloadOffset + StripLength];
        SIZE_T CipherLength = PayloadLength - StripLength;

        DecryptStream(Crypt, Cipher, CipherLength);

        /* Drop the whole basic security header (flags + MAC) so the MCS user
         * data is the bare plaintext RDP PDU, exactly as an unencrypted
         * connection delivers it to the share/input parsers. */
        MoveMemory(&Buffer[PayloadOffset], Cipher, CipherLength);

        PlainLength = CipherLength; /* new MCS user-data length */

        /* Rewrite the MCS length determinant keeping the original width so the
         * downstream parser stays byte-aligned (the PER short form still decodes
         * a two-byte encoding of a small value). */
        if (PerBytes == 2)
        {
            Buffer[McsHeader] = (UCHAR)(0x80 | (PlainLength >> 8));
            Buffer[McsHeader + 1] = (UCHAR)(PlainLength & 0xff);
        }
        else
        {
            Buffer[McsHeader] = (UCHAR)PlainLength;
        }

        Total -= StripLength;
        Buffer[2] = (UCHAR)(Total >> 8); /* TPKT length */
        Buffer[3] = (UCHAR)Total;
        *Length = Total;
    }

    return TRUE;
}

/* Fast-path input: strip MAC + RC4-decrypt when the ENCRYPTED flag is set. */
static BOOL
UnwrapFastPath(TERMSRV_RDP_CRYPT *Crypt, UCHAR *Buffer, SIZE_T *Length)
{
    SIZE_T Total = *Length;
    SIZE_T LengthBytes;
    SIZE_T HeaderLength;
    SIZE_T EventOffset;
    SIZE_T EventLength;

    if (Total < 2)
        return TRUE;
    if ((Buffer[0] & 0xC0) == 0) /* neither ENCRYPTED nor SECURE_CHECKSUM */
        return TRUE;
    if (!(Buffer[0] & TERMSRV_FASTPATH_INPUT_ENCRYPTED))
        return TRUE;

    LengthBytes = (Buffer[1] & 0x80) ? 2 : 1;
    HeaderLength = 1 + LengthBytes;
    if (Total < HeaderLength + TERMSRV_RDP_MAC_LENGTH)
        return TRUE;

    EventOffset = HeaderLength + TERMSRV_RDP_MAC_LENGTH;
    EventLength = Total - EventOffset;

    DecryptStream(Crypt, &Buffer[EventOffset], EventLength);

    /* Drop the MAC and clear the security flags in the header. */
    MoveMemory(&Buffer[HeaderLength], &Buffer[EventOffset], EventLength);
    Buffer[0] &= (UCHAR)~(TERMSRV_FASTPATH_INPUT_ENCRYPTED | TERMSRV_FASTPATH_INPUT_SECURE_CHECKSUM);

    Total -= TERMSRV_RDP_MAC_LENGTH;
    if (LengthBytes == 2)
    {
        Buffer[1] = (UCHAR)(0x80 | (Total >> 8));
        Buffer[2] = (UCHAR)(Total & 0xff);
    }
    else
    {
        Buffer[1] = (UCHAR)Total;
    }
    *Length = Total;

    return TRUE;
}

BOOL
TermSrvCryptUnwrapInbound(
    _Inout_ TERMSRV_RDP_CRYPT *Crypt,
    _Inout_updates_bytes_(*Length) UCHAR *Buffer,
    _Inout_ SIZE_T *Length)
{
    if (!Crypt->Enabled || Buffer == NULL || Length == NULL || *Length == 0)
        return TRUE;

    if (Buffer[0] == 0x03) /* TPKT */
        return UnwrapSlowPath(Crypt, Buffer, Length);

    if ((Buffer[0] & 0x03) == 0) /* fast-path */
        return UnwrapFastPath(Crypt, Buffer, Length);

    return TRUE;
}
