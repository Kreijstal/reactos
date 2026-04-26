/*
 * PROJECT:     ReactOS Terminal Services
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     RDP bitmap data helpers
 */

#include "termsrv.h"

#include <string.h>

BOOL
TermSrvConvertBgra32ToRdpBitmapData(
    _In_reads_bytes_(SourcePitch * Height) const UCHAR *Source,
    _In_ ULONG Width,
    _In_ ULONG Height,
    _In_ ULONG SourcePitch,
    _Out_writes_bytes_(Width * Height * 4) UCHAR *Destination)
{
    ULONG Row;
    ULONG RowBytes;

    if (Source == NULL || Destination == NULL || Width == 0 || Height == 0)
        return FALSE;

    RowBytes = Width * 4;
    if (SourcePitch < RowBytes)
        return FALSE;

    for (Row = 0; Row < Height; Row++)
    {
        const UCHAR *SourceRow = Source + ((Height - Row - 1) * SourcePitch);
        UCHAR *DestinationRow = Destination + (Row * RowBytes);

        memcpy(DestinationRow, SourceRow, RowBytes);
    }

    return TRUE;
}
