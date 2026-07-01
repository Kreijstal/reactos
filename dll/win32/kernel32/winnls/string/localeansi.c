/*
 * Locale support
 *
 * Copyright 1995 Martin von Loewis
 * Copyright 1998 David Lee Lambert
 * Copyright 2000 Julio Cesar Gazquez
 * Copyright 2002 Alexandre Julliard for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <k32.h>

typedef NTSTATUS (WINAPI *PRTL_NORMALIZE_STRING)(ULONG, const WCHAR *, INT, WCHAR *, INT *);

static PRTL_NORMALIZE_STRING get_rtl_normalize_string(void)
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");

    return ntdll ? (PRTL_NORMALIZE_STRING)GetProcAddress(ntdll, "RtlNormalizeString") : NULL;
}

static void FoldStringA_MapCompositeMarks(WCHAR *String, INT Length)
{
    INT i;

    for (i = 0; i < Length; i++)
    {
        switch (String[i])
        {
        case 0x0300: String[i] = '`'; break;
        case 0x0301: String[i] = 0x00b4; break;
        case 0x0302: String[i] = '^'; break;
        case 0x0303: String[i] = '~'; break;
        case 0x0308: String[i] = 0x00a8; break;
        case 0x030a: String[i] = 0x00b0; break;
        case 0x0327: String[i] = 0x00b8; break;
        }
    }
}

/*************************************************************************
 *           FoldStringA    (KERNEL32.@)
 *
 * Map characters in a string.
 *
 * PARAMS
 *  dwFlags [I] Flags controlling chars to map (MAP_ constants from "winnls.h")
 *  src     [I] String to map
 *  srclen  [I] Length of src, or -1 if src is NUL terminated
 *  dst     [O] Destination for mapped string
 *  dstlen  [I] Length of dst, or 0 to find the required length for the mapped string
 *
 * RETURNS
 *  Success: The length of the string written to dst, including the terminating NUL. If
 *           dstlen is 0, the value returned is the same, but nothing is written to dst,
 *           and dst may be NULL.
 *  Failure: 0. Use GetLastError() to determine the cause.
 */
INT WINAPI FoldStringA(DWORD dwFlags, LPCSTR src, INT srclen,
                       LPSTR dst, INT dstlen)
{
    DWORD original_flags = dwFlags;
    INT ret = 0, srclenW = 0;
    WCHAR *srcW = NULL, *dstW = NULL;

    if (!src || !srclen || dstlen < 0 || (dstlen && !dst) || src == dst)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    srclenW = MultiByteToWideChar(CP_ACP, dwFlags & MAP_COMPOSITE ? MB_COMPOSITE : 0,
                                  src, srclen, NULL, 0);
    srcW = HeapAlloc(GetProcessHeap(), 0, srclenW * sizeof(WCHAR));

    if (!srcW)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        goto FoldStringA_exit;
    }

    MultiByteToWideChar(CP_ACP, dwFlags & MAP_COMPOSITE ? MB_COMPOSITE : 0,
                        src, srclen, srcW, srclenW);

    if ((original_flags & (MAP_PRECOMPOSED | MAP_COMPOSITE | MAP_EXPAND_LIGATURES | MAP_FOLDDIGITS)) == MAP_COMPOSITE)
    {
        NTSTATUS status;
        INT normalized_len = 0;
        PRTL_NORMALIZE_STRING pRtlNormalizeString = get_rtl_normalize_string();

        if (!pRtlNormalizeString)
        {
            SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
            goto FoldStringA_exit;
        }

        status = pRtlNormalizeString(NormalizationD, srcW, srclenW, NULL, &normalized_len);
        if (status < 0)
        {
            ret = 0;
            goto FoldStringA_exit;
        }

        for (;;)
        {
            dstW = HeapAlloc(GetProcessHeap(), 0, normalized_len * sizeof(WCHAR));
            if (!dstW)
            {
                SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                goto FoldStringA_exit;
            }

            status = pRtlNormalizeString(NormalizationD, srcW, srclenW, dstW, &normalized_len);
            if (status != STATUS_BUFFER_TOO_SMALL)
                break;

            HeapFree(GetProcessHeap(), 0, dstW);
            dstW = NULL;
        }

        if (status >= 0)
        {
            FoldStringA_MapCompositeMarks(dstW, normalized_len);
            ret = WideCharToMultiByte(CP_ACP, 0, dstW, normalized_len, dst, dstlen, NULL, NULL);
            if (!ret && dstlen)
                SetLastError(ERROR_INSUFFICIENT_BUFFER);
        }
        else
        {
            ret = 0;
        }

        HeapFree(GetProcessHeap(), 0, dstW);
        goto FoldStringA_exit;
    }

    dwFlags = (dwFlags & ~MAP_PRECOMPOSED) | MAP_FOLDCZONE;

    ret = FoldStringW(dwFlags, srcW, srclenW, NULL, 0);
    if (ret && dstlen)
    {
        dstW = HeapAlloc(GetProcessHeap(), 0, ret * sizeof(WCHAR));

        if (!dstW)
        {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            goto FoldStringA_exit;
        }

        ret = FoldStringW(dwFlags, srcW, srclenW, dstW, ret);
        if (original_flags & MAP_COMPOSITE)
            FoldStringA_MapCompositeMarks(dstW, ret);
        ret = WideCharToMultiByte(CP_ACP, 0, dstW, ret, dst, dstlen, NULL, NULL);
        if (!ret)
        {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
        }
    }

    HeapFree(GetProcessHeap(), 0, dstW);

FoldStringA_exit:
    HeapFree(GetProcessHeap(), 0, srcW);
    return ret;
}

/* EOF */
