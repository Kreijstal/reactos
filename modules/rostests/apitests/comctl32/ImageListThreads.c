/*
 * PROJECT:     ReactOS API Tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Image list operations from several threads at once
 *
 * The system image list is shared by all threads of a process: shell32 adds
 * icons to it from background tasks (through IImageList and the exported
 * functions alike) while windows draw from it. Every image list operation
 * must therefore be atomic, whichever entry point is used.
 */

#define COBJMACROS
#include "wine/test.h"
#include <windows.h>
#include <commctrl.h>
#include <commoncontrols.h>

#define IMG 16
#define WRITERS 4
#define ADDS_PER_WRITER 200
#define REPLACES 3000

static HRESULT (WINAPI *pHIMAGELIST_QueryInterface)(HIMAGELIST, REFIID, void **);

static HRESULT GetImageListInterface(HIMAGELIST himl, IImageList **ppiml)
{
    if (pHIMAGELIST_QueryInterface)
        return pHIMAGELIST_QueryInterface(himl, &IID_IImageList, (void **)ppiml);
    /* An HIMAGELIST is the image list object itself */
    return IUnknown_QueryInterface((IUnknown *)himl, &IID_IImageList, (void **)ppiml);
}

static HICON CreateHalfIcon(BOOL bLeft)
{
    /* A 16x16 icon that is opaque red in one half and transparent in the other */
    BITMAPINFO bmi = { { sizeof(BITMAPINFOHEADER), IMG, -IMG, 1, 32, BI_RGB } };
    BYTE maskBits[IMG * 2]; /* 1 bpp, WORD-aligned rows of 16 pixels */
    DWORD *pColor;
    ICONINFO ii = { TRUE };
    HICON hIcon;
    HDC hdc = GetDC(NULL);
    int x, y;

    ii.hbmColor = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, (void **)&pColor, NULL, 0);
    ReleaseDC(NULL, hdc);
    for (y = 0; y < IMG; y++)
    {
        for (x = 0; x < IMG; x++)
            pColor[y * IMG + x] = ((x < IMG / 2) == bLeft) ? 0x00FF0000 : 0;
        /* mask bit 0 = opaque */
        maskBits[y * 2] = bLeft ? 0x00 : 0xFF;
        maskBits[y * 2 + 1] = bLeft ? 0xFF : 0x00;
    }
    ii.hbmMask = CreateBitmap(IMG, IMG, 1, 1, maskBits);
    hIcon = CreateIconIndirect(&ii);
    DeleteObject(ii.hbmColor);
    DeleteObject(ii.hbmMask);
    return hIcon;
}

/* Several threads append icons through IImageList at the same time */

typedef struct
{
    IImageList *piml;
    HICON hIcon;
    int indices[ADDS_PER_WRITER];
    int failures;
} ADD_CONTEXT;

static DWORD WINAPI AddThread(LPVOID param)
{
    ADD_CONTEXT *ctx = param;
    int i;

    for (i = 0; i < ADDS_PER_WRITER; i++)
    {
        if (FAILED(IImageList_ReplaceIcon(ctx->piml, -1, ctx->hIcon, &ctx->indices[i])))
        {
            ctx->indices[i] = -1;
            ctx->failures++;
        }
    }
    return 0;
}

static void TestConcurrentAdd(void)
{
    static ADD_CONTEXT ctx[WRITERS];
    static BOOL seen[WRITERS * ADDS_PER_WRITER];
    HANDLE hThreads[WRITERS];
    HIMAGELIST himl;
    IImageList *piml;
    HICON hIcon;
    int i, j, count = -1, duplicates = 0, failures = 0;
    HRESULT hr;

    himl = ImageList_Create(IMG, IMG, ILC_COLOR32 | ILC_MASK, 1, 1);
    ok(himl != NULL, "ImageList_Create failed\n");
    if (!himl)
        return;
    hr = GetImageListInterface(himl, &piml);
    ok(hr == S_OK, "GetImageListInterface hr = 0x%lx\n", hr);
    if (FAILED(hr))
    {
        ImageList_Destroy(himl);
        return;
    }
    hIcon = CreateHalfIcon(TRUE);

    ZeroMemory(seen, sizeof(seen));
    for (i = 0; i < WRITERS; i++)
    {
        ZeroMemory(&ctx[i], sizeof(ctx[i]));
        ctx[i].piml = piml;
        ctx[i].hIcon = hIcon;
        hThreads[i] = CreateThread(NULL, 0, AddThread, &ctx[i], 0, NULL);
    }
    WaitForMultipleObjects(WRITERS, hThreads, TRUE, INFINITE);

    for (i = 0; i < WRITERS; i++)
    {
        CloseHandle(hThreads[i]);
        failures += ctx[i].failures;
        for (j = 0; j < ADDS_PER_WRITER; j++)
        {
            int index = ctx[i].indices[j];
            if (index < 0 || index >= WRITERS * ADDS_PER_WRITER)
                continue;
            if (seen[index])
                duplicates++;
            seen[index] = TRUE;
        }
    }

    hr = IImageList_GetImageCount(piml, &count);
    ok(hr == S_OK, "GetImageCount hr = 0x%lx\n", hr);
    ok(failures == 0, "%d ReplaceIcon calls failed\n", failures);
    ok(duplicates == 0, "%d indices were handed out twice\n", duplicates);
    ok(count == WRITERS * ADDS_PER_WRITER, "count = %d, expected %d\n", count, WRITERS * ADDS_PER_WRITER);

    IImageList_Release(piml);
    ImageList_Destroy(himl);
    DestroyIcon(hIcon);
}

/* One thread keeps replacing image 0 while another draws it: every drawing
 * must show either the old or the new image, never half of each. */

typedef struct
{
    HIMAGELIST himl;
    IImageList *piml;
    HICON hIcons[2];
    BOOL bUseCom;
    volatile LONG done;
} REPLACE_CONTEXT;

static DWORD WINAPI ReplaceThread(LPVOID param)
{
    REPLACE_CONTEXT *ctx = param;
    int i, index;

    for (i = 0; i < REPLACES; i++)
    {
        if (ctx->bUseCom)
            IImageList_ReplaceIcon(ctx->piml, 0, ctx->hIcons[i & 1], &index);
        else
            ImageList_ReplaceIcon(ctx->himl, 0, ctx->hIcons[i & 1]);
    }
    InterlockedExchange(&ctx->done, TRUE);
    return 0;
}

static void TestDrawWhileReplacing(BOOL bUseCom)
{
    BITMAPINFO bmi = { { sizeof(BITMAPINFOHEADER), IMG, -IMG, 1, 32, BI_RGB } };
    REPLACE_CONTEXT ctx;
    HANDLE hThread;
    HDC hdc;
    HBITMAP hbm, hbmOld;
    DWORD *pBits;
    int draws = 0, torn = 0, index;

    ZeroMemory(&ctx, sizeof(ctx));
    ctx.bUseCom = bUseCom;
    /* No alpha channel: the image and the mask are written separately */
    ctx.himl = ImageList_Create(IMG, IMG, ILC_COLOR24 | ILC_MASK, 1, 1);
    ok(ctx.himl != NULL, "ImageList_Create failed\n");
    if (!ctx.himl)
        return;
    GetImageListInterface(ctx.himl, &ctx.piml);
    ok(ctx.piml != NULL, "no IImageList\n");
    ImageList_SetBkColor(ctx.himl, CLR_NONE);
    ctx.hIcons[0] = CreateHalfIcon(TRUE);
    ctx.hIcons[1] = CreateHalfIcon(FALSE);
    index = ImageList_ReplaceIcon(ctx.himl, -1, ctx.hIcons[0]);
    ok(index == 0, "index = %d\n", index);

    hdc = CreateCompatibleDC(NULL);
    hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, (void **)&pBits, NULL, 0);
    hbmOld = SelectObject(hdc, hbm);

    hThread = CreateThread(NULL, 0, ReplaceThread, &ctx, 0, NULL);
    while (!ctx.done)
    {
        DWORD left, right;

        PatBlt(hdc, 0, 0, IMG, IMG, WHITENESS);
        if (bUseCom)
        {
            IMAGELISTDRAWPARAMS ildp = { sizeof(ildp) };
            ildp.himl = ctx.himl;
            ildp.i = 0;
            ildp.hdcDst = hdc;
            ildp.rgbBk = CLR_NONE;
            ildp.rgbFg = CLR_DEFAULT;
            ildp.fStyle = ILD_NORMAL;
            IImageList_Draw(ctx.piml, &ildp);
        }
        else
        {
            ImageList_Draw(ctx.himl, 0, hdc, 0, 0, ILD_NORMAL);
        }
        GdiFlush();
        left = pBits[8 * IMG + 4] & 0xFFFFFF;
        right = pBits[8 * IMG + 12] & 0xFFFFFF;
        if (!((left == 0xFF0000 && right == 0xFFFFFF) || (left == 0xFFFFFF && right == 0xFF0000)))
        {
            if (torn++ < 3)
                trace("torn drawing: left %06lx right %06lx\n", left, right);
        }
        draws++;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    trace("%s: %d drawings during %d replacements\n", bUseCom ? "IImageList" : "exported", draws, REPLACES);
    ok(draws > 0, "no drawing done\n");
    ok(torn == 0, "%s: %d of %d drawings showed a half-replaced image\n",
       bUseCom ? "IImageList" : "exported", torn, draws);

    SelectObject(hdc, hbmOld);
    DeleteObject(hbm);
    DeleteDC(hdc);
    if (ctx.piml)
        IImageList_Release(ctx.piml);
    ImageList_Destroy(ctx.himl);
    DestroyIcon(ctx.hIcons[0]);
    DestroyIcon(ctx.hIcons[1]);
}

START_TEST(ImageListThreads)
{
    pHIMAGELIST_QueryInterface = (void *)GetProcAddress(GetModuleHandleA("comctl32.dll"), "HIMAGELIST_QueryInterface");
    TestConcurrentAdd();
    TestDrawWhileReplacing(FALSE);
    TestDrawWhileReplacing(TRUE);
}
