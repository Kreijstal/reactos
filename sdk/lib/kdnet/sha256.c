/*
 * PROJECT:     ReactOS KDNET transport
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Small allocation-free SHA-256 implementation
 */

#include "sha256.h"

#define ROTR32(Value, Count) \
    (((Value) >> (Count)) | ((Value) << (32 - (Count))))

#define CH(X, Y, Z)  (((X) & (Y)) ^ (~(X) & (Z)))
#define MAJ(X, Y, Z) (((X) & (Y)) ^ ((X) & (Z)) ^ ((Y) & (Z)))
#define BSIG0(X)     (ROTR32((X), 2) ^ ROTR32((X), 13) ^ ROTR32((X), 22))
#define BSIG1(X)     (ROTR32((X), 6) ^ ROTR32((X), 11) ^ ROTR32((X), 25))
#define SSIG0(X)     (ROTR32((X), 7) ^ ROTR32((X), 18) ^ ((X) >> 3))
#define SSIG1(X)     (ROTR32((X), 17) ^ ROTR32((X), 19) ^ ((X) >> 10))

static const uint32_t KdNetSha256Constants[64] =
{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void
KdNetCopyBytes(
    uint8_t *Destination,
    const uint8_t *Source,
    size_t Length)
{
    while (Length-- != 0)
        *Destination++ = *Source++;
}

static void
KdNetZeroBytes(
    void *Buffer,
    size_t Length)
{
    volatile uint8_t *Byte = (volatile uint8_t *)Buffer;

    while (Length-- != 0)
        *Byte++ = 0;
}

static uint32_t
KdNetReadBigEndian32(
    const uint8_t *Buffer)
{
    return ((uint32_t)Buffer[0] << 24) |
           ((uint32_t)Buffer[1] << 16) |
           ((uint32_t)Buffer[2] << 8) |
           (uint32_t)Buffer[3];
}

static void
KdNetWriteBigEndian32(
    uint8_t *Buffer,
    uint32_t Value)
{
    Buffer[0] = (uint8_t)(Value >> 24);
    Buffer[1] = (uint8_t)(Value >> 16);
    Buffer[2] = (uint8_t)(Value >> 8);
    Buffer[3] = (uint8_t)Value;
}

static void
KdNetSha256Transform(
    KDNET_SHA256_CONTEXT *Context,
    const uint8_t Block[64])
{
    uint32_t Words[64];
    uint32_t A, B, C, D, E, F, G, H;
    uint32_t Temporary1, Temporary2;
    size_t Index;

    for (Index = 0; Index < 16; ++Index)
        Words[Index] = KdNetReadBigEndian32(Block + Index * 4);

    for (Index = 16; Index < 64; ++Index)
    {
        Words[Index] = SSIG1(Words[Index - 2]) + Words[Index - 7] +
                       SSIG0(Words[Index - 15]) + Words[Index - 16];
    }

    A = Context->State[0];
    B = Context->State[1];
    C = Context->State[2];
    D = Context->State[3];
    E = Context->State[4];
    F = Context->State[5];
    G = Context->State[6];
    H = Context->State[7];

    for (Index = 0; Index < 64; ++Index)
    {
        Temporary1 = H + BSIG1(E) + CH(E, F, G) +
                     KdNetSha256Constants[Index] + Words[Index];
        Temporary2 = BSIG0(A) + MAJ(A, B, C);
        H = G;
        G = F;
        F = E;
        E = D + Temporary1;
        D = C;
        C = B;
        B = A;
        A = Temporary1 + Temporary2;
    }

    Context->State[0] += A;
    Context->State[1] += B;
    Context->State[2] += C;
    Context->State[3] += D;
    Context->State[4] += E;
    Context->State[5] += F;
    Context->State[6] += G;
    Context->State[7] += H;

    KdNetZeroBytes(Words, sizeof(Words));
}

void
KdNetSha256Initialize(
    KDNET_SHA256_CONTEXT *Context)
{
    Context->State[0] = 0x6a09e667;
    Context->State[1] = 0xbb67ae85;
    Context->State[2] = 0x3c6ef372;
    Context->State[3] = 0xa54ff53a;
    Context->State[4] = 0x510e527f;
    Context->State[5] = 0x9b05688c;
    Context->State[6] = 0x1f83d9ab;
    Context->State[7] = 0x5be0cd19;
    Context->ByteCount = 0;
    KdNetZeroBytes(Context->Buffer, sizeof(Context->Buffer));
}

void
KdNetSha256Update(
    KDNET_SHA256_CONTEXT *Context,
    const uint8_t *Input,
    size_t InputLength)
{
    size_t BufferOffset;
    size_t CopyLength;

    if (InputLength == 0)
        return;

    BufferOffset = (size_t)(Context->ByteCount & 63);
    Context->ByteCount += InputLength;

    if (BufferOffset != 0)
    {
        CopyLength = 64 - BufferOffset;
        if (CopyLength > InputLength)
            CopyLength = InputLength;
        KdNetCopyBytes(Context->Buffer + BufferOffset, Input, CopyLength);
        Input += CopyLength;
        InputLength -= CopyLength;
        BufferOffset += CopyLength;
        if (BufferOffset == 64)
            KdNetSha256Transform(Context, Context->Buffer);
    }

    while (InputLength >= 64)
    {
        KdNetSha256Transform(Context, Input);
        Input += 64;
        InputLength -= 64;
    }

    if (InputLength != 0)
        KdNetCopyBytes(Context->Buffer, Input, InputLength);
}

void
KdNetSha256Finish(
    KDNET_SHA256_CONTEXT *Context,
    uint8_t Digest[32])
{
    uint64_t BitCount;
    size_t Offset;
    size_t Index;

    BitCount = Context->ByteCount << 3;
    Offset = (size_t)(Context->ByteCount & 63);
    Context->Buffer[Offset++] = 0x80;

    if (Offset > 56)
    {
        while (Offset < 64)
            Context->Buffer[Offset++] = 0;
        KdNetSha256Transform(Context, Context->Buffer);
        Offset = 0;
    }

    while (Offset < 56)
        Context->Buffer[Offset++] = 0;

    for (Index = 0; Index < 8; ++Index)
        Context->Buffer[63 - Index] = (uint8_t)(BitCount >> (Index * 8));

    KdNetSha256Transform(Context, Context->Buffer);

    for (Index = 0; Index < 8; ++Index)
        KdNetWriteBigEndian32(Digest + Index * 4, Context->State[Index]);

    KdNetZeroBytes(Context, sizeof(*Context));
}
