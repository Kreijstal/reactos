/* NTFS 1.1 restart pages shared by the formatter and offline checker.
 * GPL-2.0-or-later. See [MS-FSCC] and the NTFS LFS restart layout.
 */
#if defined(NTOS_MODE_USER)
#include <ndk/umtypes.h>
#include <ndk/rtlfuncs.h>
#define memset(d, v, n) RtlFillMemory((d), (n), (v))
#else
#include <string.h>
#endif
#include "ntfs_types.h"
#include "restart.h"

static void
Put16(UCHAR *Buffer, USHORT Value)
{
    Buffer[0] = (UCHAR)Value;
    Buffer[1] = (UCHAR)(Value >> 8);
}

static void
Put32(UCHAR *Buffer, ULONG Value)
{
    Put16(Buffer, (USHORT)Value);
    Put16(Buffer + 2, (USHORT)(Value >> 16));
}

static void
Put64(UCHAR *Buffer, ULONGLONG Value)
{
    Put32(Buffer, (ULONG)Value);
    Put32(Buffer + 4, (ULONG)(Value >> 32));
}

int
NtfsBuildCleanRestart(UCHAR *Page, ULONG PageSize, ULONG SectorSize,
                      ULONGLONG FileSize, ULONGLONG CurrentLsn)
{
    UCHAR *Area, *Client;
    ULONG i, Count, Bits = 0;
    ULONGLONG Size = FileSize;

    if (Page == NULL || SectorSize < 512 || PageSize < SectorSize ||
        PageSize % SectorSize != 0 || (PageSize & (PageSize - 1)) != 0 ||
        FileSize < 4ULL * PageSize)
        return -1;
    Count = PageSize / SectorSize + 1;
    if (Count > (0x40 - 0x28) / 2 || PageSize < 0x110)
        return -1;

    while (Size != 0)
    {
        ++Bits;
        Size >>= 1;
    }
    memset(Page, 0, PageSize);
    Put32(Page, 0x52545352);          /* RSTR */
    Put16(Page + 4, 0x28);           /* USA offset */
    Put16(Page + 6, (USHORT)Count);
    Put32(Page + 0x10, PageSize);    /* System page size */
    Put32(Page + 0x14, PageSize);    /* Log page size */
    Put16(Page + 0x18, 0x40);        /* Restart area offset */
    Put16(Page + 0x1A, 1);           /* Minor version */
    Put16(Page + 0x1C, 1);           /* Major version */

    Area = Page + 0x40;
    Put64(Area, CurrentLsn);
    Put16(Area + 8, 1);              /* One registered client */
    Put16(Area + 0xA, 0xFFFF);       /* Empty free-client list */
    Put16(Area + 0xC, 0);            /* NTFS is client zero */
    Put16(Area + 0xE, 2);            /* Clean shutdown */
    Put32(Area + 0x10, 67 - Bits);
    Put16(Area + 0x14, 0xD0);        /* Includes the client array */
    Put16(Area + 0x16, 0x30);        /* Client array offset */
    Put64(Area + 0x18, FileSize);
    Put16(Area + 0x24, 0x30);        /* LFS record header length */
    Put16(Area + 0x26, 0x40);        /* Log-page data offset */
    Put32(Area + 0x28, 1);           /* Restart open-log count */

    Client = Area + 0x30;
    Put16(Client + 0x10, 0xFFFF);
    Put16(Client + 0x12, 0xFFFF);
    Put16(Client + 0x14, 1);         /* Client sequence number */
    Put32(Client + 0x1C, 8);
    Put16(Client + 0x20, 'N');
    Put16(Client + 0x22, 'T');
    Put16(Client + 0x24, 'F');
    Put16(Client + 0x26, 'S');

    Put16(Page + 0x28, 1);
    for (i = 1; i < Count; ++i)
    {
        UCHAR *Tail = Page + i * SectorSize - 2;
        Page[0x28 + 2 * i] = Tail[0];
        Page[0x29 + 2 * i] = Tail[1];
        Put16(Tail, 1);
    }
    return 0;
}
