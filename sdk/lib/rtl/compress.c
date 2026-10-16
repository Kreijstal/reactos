/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS system libraries
 * PURPOSE:         Compression and decompression functions
 * FILE:            lib/rtl/compress.c
 * PROGRAMER:       Eric Kohl
                    Sebastian Lackner
                    Michael Müller
 */

/* INCLUDES *****************************************************************/

#include <rtl.h>

#define NDEBUG
#include <debug.h>

/* MACROS *******************************************************************/

#define COMPRESSION_FORMAT_MASK  0x00FF
#define COMPRESSION_ENGINE_MASK  0xFF00




/* FUNCTIONS ****************************************************************/

/* Based on Wine Staging */

/* decompress a single LZNT1 chunk */
static PUCHAR lznt1_decompress_chunk(UCHAR *dst, ULONG dst_size, UCHAR *src, ULONG src_size)
{
    UCHAR *src_cur, *src_end, *dst_cur, *dst_end;
    ULONG displacement_bits, length_bits;
    ULONG code_displacement, code_length;
    WORD flags, code;

    src_cur = src;
    src_end = src + src_size;
    dst_cur = dst;
    dst_end = dst + dst_size;

    /* Partial decompression is no error on Windows. */
    while (src_cur < src_end && dst_cur < dst_end)
    {
        /* read flags header */
        flags = 0x8000 | *src_cur++;

        /* parse following 8 entities, either uncompressed data or backwards reference */
        while ((flags & 0xFF00) && src_cur < src_end)
        {
            if (flags & 1)
            {
                /* backwards reference */
                if (src_cur + sizeof(WORD) > src_end)
                    return NULL;
                code = *(WORD *)src_cur;
                src_cur += sizeof(WORD);

                /* find length / displacement bits */
                for (displacement_bits = 12; displacement_bits > 4; displacement_bits--)
                    if ((1 << (displacement_bits - 1)) < (ULONG)(dst_cur - dst)) break;
                length_bits       = 16 - displacement_bits;
                code_length       = (code & ((1 << length_bits) - 1)) + 3;
                code_displacement = (code >> length_bits) + 1;

                /* ensure reference is valid */
                if (dst_cur < dst + code_displacement)
                    return NULL;

                /* copy bytes of chunk - we can't use memcpy()
                 * since source and dest can be overlapping */
                while (code_length--)
                {
                    if (dst_cur >= dst_end) return dst_cur;
                    *dst_cur = *(dst_cur - code_displacement);
                    dst_cur++;
                }
            }
            else
            {
                /* uncompressed data */
                if (dst_cur >= dst_end) return dst_cur;
                *dst_cur++ = *src_cur++;
            }
            flags >>= 1;
        }

    }

    return dst_cur;
}

/* decompress data encoded with LZNT1 */
static NTSTATUS lznt1_decompress(UCHAR *dst, ULONG dst_size, UCHAR *src, ULONG src_size,
                                 ULONG offset, ULONG *final_size, UCHAR *workspace)
{
    UCHAR *src_cur = src, *src_end = src + src_size;
    UCHAR *dst_cur = dst, *dst_end = dst + dst_size;
    ULONG chunk_size, block_size;
    ULONG decompressed_size;
    WORD chunk_header;
    UCHAR *ptr;

    if (src_cur + sizeof(WORD) > src_end)
        return STATUS_BAD_COMPRESSION_BUFFER;

    /* skip over chunks which have a big distance (>= 0x1000) to the destination offset */
    while (offset >= 0x1000 && src_cur + sizeof(WORD) <= src_end)
    {
        /* read chunk header and extract size */
        chunk_header = *(WORD *)src_cur;
        src_cur += sizeof(WORD);
        if (!chunk_header) goto out;
        chunk_size = (chunk_header & 0xFFF) + 1;

        /* ensure we have enough buffer to process chunk */
        if (src_cur + chunk_size > src_end)
            return STATUS_BAD_COMPRESSION_BUFFER;

        src_cur += chunk_size;
        offset  -= 0x1000;
    }

    if (workspace && src_cur + sizeof(WORD) <= src_end &&
        *(WORD *)src_cur && !(*(WORD *)src_cur & 0x8000))
    {
        return STATUS_BAD_COMPRESSION_BUFFER;
    }

    /* this chunk can be included partially */
    if (offset && src_cur + sizeof(WORD) <= src_end)
    {
        /* read chunk header and extract size */
        chunk_header = *(WORD *)src_cur;
        src_cur += sizeof(WORD);
        if (!chunk_header)
        {
            block_size = min(0x1000 - offset, dst_end - dst_cur);
            memset(dst_cur, 0, block_size);
            dst_cur += block_size;
            goto out;
        }
        chunk_size = (chunk_header & 0xFFF) + 1;

        /* ensure we have enough buffer to process chunk */
        if (src_cur + chunk_size > src_end)
            return STATUS_BAD_COMPRESSION_BUFFER;

        if (dst_cur >= dst_end)
            goto out;

        if (chunk_header & 0x8000)
        {
            /* compressed chunk */
            if (!workspace) return STATUS_ACCESS_VIOLATION;
            ptr = lznt1_decompress_chunk(workspace, 0x1000, src_cur, chunk_size);
            if (!ptr) return STATUS_BAD_COMPRESSION_BUFFER;
            decompressed_size = ptr - workspace;
            if (decompressed_size > offset)
            {
                block_size = min(decompressed_size - offset, dst_end - dst_cur);
                memcpy(dst_cur, workspace + offset, block_size);
                dst_cur += block_size;
            }
            else if (!decompressed_size || offset == 0xFFF)
            {
                block_size = min(0x1000 - offset, dst_end - dst_cur);
                memset(dst_cur, 0, block_size);
                dst_cur += block_size;
            }
        }
        else
        {
            /* uncompressed chunk */
            if (chunk_size > offset)
            {
                block_size = min(chunk_size - offset, dst_end - dst_cur);
                memcpy(dst_cur, src_cur + offset, block_size);
                dst_cur += block_size;
            }
            else if (offset == 0xFFF)
            {
                block_size = min(0x1000 - offset, dst_end - dst_cur);
                memset(dst_cur, 0, block_size);
                dst_cur += block_size;
            }
        }

        src_cur += chunk_size;
    }

    /* handle remaining chunks */
    while (src_cur + sizeof(WORD) <= src_end)
    {
        /* read chunk header and extract size */
        chunk_header = *(WORD *)src_cur;
        src_cur += sizeof(WORD);
        if (!chunk_header) goto out;
        chunk_size = (chunk_header & 0xFFF) + 1;

        if (src_cur + chunk_size > src_end)
            return STATUS_BAD_COMPRESSION_BUFFER;

        /* add padding if required */
        block_size = ((dst_cur - dst) + offset) & 0xFFF;
        if (block_size)
        {
            block_size = 0x1000 - block_size;
            if (dst_cur + block_size >= dst_end)
                goto out;
            memset(dst_cur, 0, block_size);
            dst_cur += block_size;
        }

        if (dst_cur >= dst_end)
            goto out;

        if (chunk_header & 0x8000)
        {
            /* compressed chunk */
            dst_cur = lznt1_decompress_chunk(dst_cur, dst_end - dst_cur, src_cur, chunk_size);
            if (!dst_cur) return STATUS_BAD_COMPRESSION_BUFFER;
        }
        else
        {
            /* uncompressed chunk */
            block_size = min(chunk_size, dst_end - dst_cur);
            memcpy(dst_cur, src_cur, block_size);
            dst_cur += block_size;
        }

        src_cur += chunk_size;
    }

out:
    if (final_size)
        *final_size = dst_cur - dst;

    return STATUS_SUCCESS;

}


/* LZNT1 compression: what the workspace of a chunk holds */
#define LZNT1_CHUNK_SIZE    0x1000
#define LZNT1_HASH_SIZE     0x1000
#define LZNT1_NO_POSITION   0xFFFF
#define LZNT1_MAX_CHAIN     64

typedef struct _LZNT1_WORKSPACE
{
    USHORT Head[LZNT1_HASH_SIZE];   /* last position with each hash */
    USHORT Previous[LZNT1_CHUNK_SIZE]; /* the position before it with the same hash */
} LZNT1_WORKSPACE, *PLZNT1_WORKSPACE;

C_ASSERT(sizeof(LZNT1_WORKSPACE) <= 0x8010);

static ULONG
lznt1_hash(const UCHAR *p)
{
    return ((p[0] << 4) ^ (p[1] << 2) ^ p[2]) & (LZNT1_HASH_SIZE - 1);
}

/* The split of a back reference at chunk position pos, as the decompressor reads it */
static ULONG
lznt1_displacement_bits(ULONG pos)
{
    ULONG bits;

    for (bits = 12; bits > 4; bits--)
        if ((1UL << (bits - 1)) < pos) break;
    return bits;
}

static VOID
lznt1_insert(PLZNT1_WORKSPACE ws, const UCHAR *chunk, ULONG pos, ULONG size)
{
    ULONG hash;

    if (pos + 3 > size)
        return;
    hash = lznt1_hash(chunk + pos);
    ws->Previous[pos] = ws->Head[hash];
    ws->Head[hash] = (USHORT)pos;
}

/* The longest earlier match for chunk + pos within reach of a back reference there */
static ULONG
lznt1_find_match(PLZNT1_WORKSPACE ws, const UCHAR *chunk, ULONG pos, ULONG size, ULONG *displacement)
{
    ULONG bits = lznt1_displacement_bits(pos);
    ULONG max_displacement = 1UL << bits;
    ULONG max_length = min((1UL << (16 - bits)) + 2, size - pos);
    ULONG best = 0, steps = 0;
    USHORT candidate;

    if (max_length < 3)
        return 0;

    for (candidate = ws->Head[lznt1_hash(chunk + pos)];
         candidate != LZNT1_NO_POSITION && steps < LZNT1_MAX_CHAIN;
         candidate = ws->Previous[candidate], steps++)
    {
        ULONG length = 0;

        if (pos - candidate > max_displacement)
            break;

        while (length < max_length && chunk[candidate + length] == chunk[pos + length])
            length++;

        if (length > best)
        {
            best = length;
            *displacement = pos - candidate;
            if (best == max_length)
                break;
        }
    }

    return (best >= 3) ? best : 0;
}

/* Compresses one chunk; returns the size of its data, or 0 if it doesn't get smaller */
static ULONG
lznt1_compress_chunk(PLZNT1_WORKSPACE ws, const UCHAR *chunk, ULONG size, UCHAR *out, ULONG out_size)
{
    ULONG pos = 0, used = 0;

    memset(ws->Head, 0xFF, sizeof(ws->Head));

    while (pos < size)
    {
        ULONG flag_byte = used++;
        UCHAR flags = 0;
        ULONG bit;

        if (used > out_size)
            return 0;

        for (bit = 0; bit < 8 && pos < size; bit++)
        {
            ULONG displacement = 0;
            ULONG length = lznt1_find_match(ws, chunk, pos, size, &displacement);

            if (length != 0)
            {
                ULONG length_bits = 16 - lznt1_displacement_bits(pos);
                USHORT token = (USHORT)(((displacement - 1) << length_bits) | (length - 3));

                if (used + sizeof(USHORT) > out_size)
                    return 0;
                out[used++] = token & 0xFF;
                out[used++] = token >> 8;
                flags |= 1 << bit;

                while (length--)
                    lznt1_insert(ws, chunk, pos++, size);
            }
            else
            {
                if (used + 1 > out_size)
                    return 0;
                out[used++] = chunk[pos];
                lznt1_insert(ws, chunk, pos++, size);
            }
        }

        out[flag_byte] = flags;
    }

    return (used < size) ? used : 0;
}

static NTSTATUS
RtlpCompressBufferLZNT1(UCHAR *src, ULONG src_size, UCHAR *dst, ULONG dst_size,
                        ULONG chunk_size, ULONG *final_size, UCHAR *workspace)
{
        UCHAR *src_cur = src, *src_end = src + src_size;
        UCHAR *dst_cur = dst, *dst_end = dst + dst_size;
        ULONG block_size, compressed_size;

        while (src_cur < src_end)
        {
            /* determine size of current chunk */
            block_size = min(LZNT1_CHUNK_SIZE, src_end - src_cur);
            if (dst_cur + sizeof(WORD) > dst_end)
                return STATUS_BUFFER_TOO_SMALL;

            /* a chunk that compresses goes in compressed, any other as it is */
            compressed_size = workspace ?
                lznt1_compress_chunk((PLZNT1_WORKSPACE)workspace, src_cur, block_size,
                                     dst_cur + sizeof(WORD), (ULONG)(dst_end - dst_cur - sizeof(WORD))) : 0;
            if (compressed_size != 0)
            {
                *(WORD *)dst_cur = 0xB000 | (compressed_size - 1);
                dst_cur += sizeof(WORD) + compressed_size;
            }
            else
            {
                if (dst_cur + sizeof(WORD) + block_size > dst_end)
                    return STATUS_BUFFER_TOO_SMALL;

                *(WORD *)dst_cur = 0x3000 | (block_size - 1);
                dst_cur += sizeof(WORD);
                memcpy(dst_cur, src_cur, block_size);
                dst_cur += block_size;
            }
            src_cur += block_size;
        }

        if (final_size)
            *final_size = dst_cur - dst;

        return STATUS_SUCCESS;
}


static NTSTATUS
RtlpWorkSpaceSizeLZNT1(USHORT Engine,
                       PULONG BufferAndWorkSpaceSize,
                       PULONG FragmentWorkSpaceSize)
{
   if (Engine == COMPRESSION_ENGINE_STANDARD)
   {
      *BufferAndWorkSpaceSize = 0x8010;
      *FragmentWorkSpaceSize = 0x1000;
      return(STATUS_SUCCESS);
   }
   else if (Engine == COMPRESSION_ENGINE_MAXIMUM)
   {
      *BufferAndWorkSpaceSize = 0x10;
      *FragmentWorkSpaceSize = 0x1000;
      return(STATUS_SUCCESS);
   }

   return(STATUS_NOT_SUPPORTED);
}


/*
 * @implemented
 */
NTSTATUS NTAPI
RtlCompressBuffer(IN USHORT CompressionFormatAndEngine,
                  IN PUCHAR UncompressedBuffer,
                  IN ULONG UncompressedBufferSize,
                  OUT PUCHAR CompressedBuffer,
                  IN ULONG CompressedBufferSize,
                  IN ULONG UncompressedChunkSize,
                  OUT PULONG FinalCompressedSize,
                  IN PVOID WorkSpace)
{
   USHORT Format = CompressionFormatAndEngine & COMPRESSION_FORMAT_MASK;
   /* USHORT Engine = CompressionFormatAndEngine & COMPRESSION_ENGINE_MASK; */

   if ((Format == COMPRESSION_FORMAT_NONE) ||
         (Format == COMPRESSION_FORMAT_DEFAULT))
      return(STATUS_INVALID_PARAMETER);

   if (Format == COMPRESSION_FORMAT_LZNT1)
      return(RtlpCompressBufferLZNT1(UncompressedBuffer,
                                     UncompressedBufferSize,
                                     CompressedBuffer,
                                     CompressedBufferSize,
                                     UncompressedChunkSize,
                                     FinalCompressedSize,
                                     WorkSpace));

   return(STATUS_UNSUPPORTED_COMPRESSION);
}


/*
 * @unimplemented
 */
NTSTATUS NTAPI
RtlCompressChunks(IN PUCHAR UncompressedBuffer,
                  IN ULONG UncompressedBufferSize,
                  OUT PUCHAR CompressedBuffer,
                  IN ULONG CompressedBufferSize,
                  IN OUT PCOMPRESSED_DATA_INFO CompressedDataInfo,
                  IN ULONG CompressedDataInfoLength,
                  IN PVOID WorkSpace)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

/*
 * @unimplemented
 */
NTSTATUS NTAPI
RtlDecompressChunks(OUT PUCHAR UncompressedBuffer,
                    IN ULONG UncompressedBufferSize,
                    IN PUCHAR CompressedBuffer,
                    IN ULONG CompressedBufferSize,
                    IN PUCHAR CompressedTail,
                    IN ULONG CompressedTailSize,
                    IN PCOMPRESSED_DATA_INFO CompressedDataInfo)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

/*
 * @implemented
 */
NTSTATUS NTAPI
RtlDecompressFragment(IN USHORT format,
                      OUT PUCHAR uncompressed,
                      IN ULONG uncompressed_size,
                      IN PUCHAR compressed,
                      IN ULONG compressed_size,
                      IN ULONG offset,
                      OUT PULONG final_size,
                      IN PVOID workspace)
{
    DPRINT("0x%04x, %p, %u, %p, %u, %u, %p, %p :stub\n", format, uncompressed,
           uncompressed_size, compressed, compressed_size, offset, final_size, workspace);

    switch (format & ~COMPRESSION_ENGINE_MAXIMUM)
    {
        case COMPRESSION_FORMAT_LZNT1:
            return lznt1_decompress(uncompressed, uncompressed_size, compressed,
                                    compressed_size, offset, final_size, workspace);

        case COMPRESSION_FORMAT_NONE:
        case COMPRESSION_FORMAT_DEFAULT:
            return STATUS_INVALID_PARAMETER;

        default:
            DPRINT1("format %d not implemented\n", format);
            return STATUS_UNSUPPORTED_COMPRESSION;
    }
}

/*
 * @implemented
 */
NTSTATUS NTAPI
RtlDecompressBuffer(IN USHORT CompressionFormat,
                    OUT PUCHAR UncompressedBuffer,
                    IN ULONG UncompressedBufferSize,
                    IN PUCHAR CompressedBuffer,
                    IN ULONG CompressedBufferSize,
                    OUT PULONG FinalUncompressedSize)
{
    return RtlDecompressFragment(CompressionFormat, UncompressedBuffer, UncompressedBufferSize,
                                 CompressedBuffer, CompressedBufferSize, 0, FinalUncompressedSize, NULL);
}

/*
 * @unimplemented
 */
NTSTATUS NTAPI
RtlDescribeChunk(IN USHORT CompressionFormat,
                 IN OUT PUCHAR *CompressedBuffer,
                 IN PUCHAR EndOfCompressedBufferPlus1,
                 OUT PUCHAR *ChunkBuffer,
                 OUT PULONG ChunkSize)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}


/*
 * @unimplemented
 */
NTSTATUS NTAPI
RtlGetCompressionWorkSpaceSize(IN USHORT CompressionFormatAndEngine,
                               OUT PULONG CompressBufferAndWorkSpaceSize,
                               OUT PULONG CompressFragmentWorkSpaceSize)
{
   USHORT Format = CompressionFormatAndEngine & COMPRESSION_FORMAT_MASK;
   USHORT Engine = CompressionFormatAndEngine & COMPRESSION_ENGINE_MASK;

   if ((Format == COMPRESSION_FORMAT_NONE) ||
         (Format == COMPRESSION_FORMAT_DEFAULT))
      return(STATUS_INVALID_PARAMETER);

   if (Format == COMPRESSION_FORMAT_LZNT1)
      return(RtlpWorkSpaceSizeLZNT1(Engine,
                                    CompressBufferAndWorkSpaceSize,
                                    CompressFragmentWorkSpaceSize));

   return(STATUS_UNSUPPORTED_COMPRESSION);
}



/*
 * @unimplemented
 */
NTSTATUS NTAPI
RtlReserveChunk(IN USHORT CompressionFormat,
                IN OUT PUCHAR *CompressedBuffer,
                IN PUCHAR EndOfCompressedBufferPlus1,
                OUT PUCHAR *ChunkBuffer,
                IN ULONG ChunkSize)
{
    UNIMPLEMENTED;
    return STATUS_NOT_IMPLEMENTED;
}

/* EOF */
