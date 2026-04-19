/*
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/filesystem/ntfs/lznt1.c
 * PURPOSE:          NTFS LZNT1 decompression codec (compressed $DATA read path).
 *
 * This module implements NtfsLznt1Decompress, the pure in-memory
 * codec that turns one NTFS compression unit of raw compressed bytes
 * back into uncompressed bytes.  A compression unit is typically 16
 * KiB (16 clusters of 1 KiB, or 4 clusters of 4 KiB); the codec is
 * independent of the cluster size — it only cares about byte buffers.
 *
 * This is a fresh re-implementation from public LZNT1 documentation:
 *
 *   - [MS-XCA] Xpress Compression Algorithm Specification
 *     (section for the LZNT1 variant), Microsoft Open Specifications.
 *   - ntfs-3g libntfs-3g/compress.c (GPL-2.0+, license-compatible with
 *     the rest of this driver) — referenced for algorithmic shape
 *     (block header format, symbol-split heuristic) but not copied.
 *
 * NOT copied from Linux ntfs3's fs/ntfs3/lznt.c (GPL-2.0-only,
 * license-incompatible with GPL-2.0+ ReactOS NTFS).  Consult MS
 * spec or ntfs-3g if clarifications are needed.
 *
 * Slice 1a (issue #35): decompression only.  Slice 1b (separate
 * issue) will add NtfsLznt1Compress for the compressed-write path.
 *
 * On-disk layout recap:
 *
 *   A compression unit is a sequence of variable-size "blocks".
 *   Each block is prefixed by a 2-byte little-endian header:
 *
 *       bits  0..11  block body size minus 1 (so 1..4096 body bytes)
 *       bits 12..14  block signature (always 0b011 for NTFS)
 *       bit     15   1 = compressed block body, 0 = verbatim copy
 *
 *   A header value of 0x0000 is the end-of-CU marker: the rest of
 *   the uncompressed CU is implicitly zero-filled.
 *
 *   A compressed block body is a sequence of token groups.  Each
 *   group starts with a 1-byte flag; the 8 bits of the flag (LSB
 *   first) describe the next 8 tokens:
 *
 *       flag_bit = 0 : next token is one literal byte
 *       flag_bit = 1 : next token is a 2-byte little-endian
 *                      back-reference (offset, length) pair
 *
 *   A back-reference token is decoded against the current
 *   decompressed-position within this block using a variable bit
 *   split: short positions get a 4-bit length / 12-bit offset
 *   split, and as the block fills, the split slides one bit per
 *   power-of-two boundary up to 12-bit length / 4-bit offset.
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/* Compute the length/offset bit split for a back-reference token,
 * given the number of bytes already decompressed in the current
 * block.  Mirrors the rule in [MS-XCA] and ntfs-3g's get_position().
 * Returns the number of LENGTH bits (4..12); the OFFSET bits are
 * 16 - length_bits. */
static ULONG
NtfsLznt1GetLengthBits(ULONG BlockPosition)
{
    ULONG OffsetBits = 4;
    ULONG Limit = 16; /* 1 << 4 */
    while (OffsetBits < 12 && Limit < BlockPosition)
    {
        OffsetBits++;
        Limit <<= 1;
    }
    return 16 - OffsetBits;
}

/**
 * @name NtfsLznt1Decompress
 * @implemented
 *
 * Decode one NTFS compression unit from raw compressed bytes into
 * uncompressed bytes.  The caller sizes OutLen to the full
 * compression-unit length (typically 16 KiB); short CUs are
 * zero-padded implicitly via the end-of-CU marker.
 *
 * @param In        Source buffer (compressed CU bytes).
 * @param InLen     Length of source buffer, in bytes.
 * @param Out       Destination buffer (uncompressed bytes).
 * @param OutLen    Size of Out, in bytes (typically CU_clusters * bytes_per_cluster).
 * @param OutUsed   Optional: receives number of logical bytes actually
 *                  written to Out before the end-of-CU marker.  Out[0..*OutUsed]
 *                  holds the decompressed prefix; Out[*OutUsed..OutLen] is
 *                  zero-padded by this routine.
 *
 * @return STATUS_SUCCESS on a well-formed CU,
 *         STATUS_INVALID_PARAMETER for NULL buffers / zero lengths,
 *         STATUS_DATA_ERROR for malformed block headers, out-of-range
 *         back-references, or tokens extending past the block body.
 */
NTSTATUS
NtfsLznt1Decompress(const UCHAR *In,
                    ULONG InLen,
                    UCHAR *Out,
                    ULONG OutLen,
                    PULONG OutUsed)
{
    ULONG InPos = 0;
    ULONG OutPos = 0;

    if (In == NULL || Out == NULL || InLen == 0 || OutLen == 0)
        return STATUS_INVALID_PARAMETER;

    /* Zero the full output up front; the end-of-CU marker below then
     * only needs to stop consuming — trailing bytes are already zero.
     * This also guarantees the caller sees a defined buffer on early
     * error returns. */
    RtlZeroMemory(Out, OutLen);

    while (InPos + 2 <= InLen)
    {
        USHORT Header;
        ULONG  BodyLen;
        BOOLEAN Compressed;
        ULONG  BlockStart;

        /* Block header is a 2-byte little-endian USHORT. */
        Header = (USHORT)(In[InPos] | ((USHORT)In[InPos + 1] << 8));
        InPos += 2;

        /* Zero header = end-of-CU: remainder of Out is implicitly zero. */
        if (Header == 0)
            break;

        BodyLen = (ULONG)(Header & 0x0FFF) + 1;
        Compressed = (Header & 0x8000) ? TRUE : FALSE;

        if (InPos + BodyLen > InLen)
            return STATUS_DATA_ERROR;

        BlockStart = OutPos;

        if (!Compressed)
        {
            /* Verbatim block: copy BodyLen bytes straight across. */
            if (OutPos + BodyLen > OutLen)
                return STATUS_DATA_ERROR;
            RtlCopyMemory(Out + OutPos, In + InPos, BodyLen);
            InPos  += BodyLen;
            OutPos += BodyLen;
            continue;
        }

        /* Compressed block: walk token groups until BodyLen consumed. */
        {
            ULONG BlockEnd = InPos + BodyLen;
            while (InPos < BlockEnd)
            {
                UCHAR Flags = In[InPos++];
                ULONG Bit;

                for (Bit = 0; Bit < 8 && InPos < BlockEnd; Bit++)
                {
                    if ((Flags & (1u << Bit)) == 0)
                    {
                        /* Literal byte. */
                        if (OutPos >= OutLen)
                            return STATUS_DATA_ERROR;
                        Out[OutPos++] = In[InPos++];
                    }
                    else
                    {
                        /* Back-reference token (2 bytes little-endian). */
                        USHORT Token;
                        ULONG  LengthBits;
                        ULONG  LengthMask;
                        ULONG  MatchLen;
                        ULONG  MatchOff;
                        ULONG  BlockPos;
                        ULONG  Src;
                        ULONG  i;

                        if (InPos + 2 > BlockEnd)
                            return STATUS_DATA_ERROR;

                        Token = (USHORT)(In[InPos] | ((USHORT)In[InPos + 1] << 8));
                        InPos += 2;

                        BlockPos   = OutPos - BlockStart;
                        LengthBits = NtfsLznt1GetLengthBits(BlockPos);
                        LengthMask = (1u << LengthBits) - 1u;

                        MatchLen = (ULONG)(Token & LengthMask) + 3;
                        MatchOff = (ULONG)(Token >> LengthBits) + 1;

                        /* Back-reference must land inside the current block. */
                        if (MatchOff > BlockPos || BlockPos == 0)
                            return STATUS_DATA_ERROR;

                        if (OutPos + MatchLen > OutLen)
                            return STATUS_DATA_ERROR;

                        /* Byte-wise copy: intentional overlap is legal and is
                         * how LZNT1 encodes run-length sequences. */
                        Src = OutPos - MatchOff;
                        for (i = 0; i < MatchLen; i++)
                            Out[OutPos + i] = Out[Src + i];
                        OutPos += MatchLen;
                    }
                }
            }

            if (InPos != BlockEnd)
                return STATUS_DATA_ERROR;
        }
    }

    if (OutUsed != NULL)
        *OutUsed = OutPos;

    return STATUS_SUCCESS;
}

/* EOF */
