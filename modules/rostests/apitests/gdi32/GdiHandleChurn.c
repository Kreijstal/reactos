/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     GDI process cleanup while other threads create and delete GDI objects
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 */

#include "precomp.h"

/*
 * The GDI handle table is shared by all processes and is lock free: entries are
 * inserted and freed concurrently by every thread. Process exit (win32k
 * GDI_CleanupForProcess) must not inspect other processes' entries as if the
 * table were quiescent. This test exits many GDI processes while threads of
 * this process churn GDI objects; a broken cleanup crashes the kernel.
 */

#define CHURN_THREADS 2
#define CHILD_COUNT 200

static volatile LONG g_StopChurn;

static DWORD WINAPI
ChurnThread(PVOID Param)
{
    HGDIOBJ ahObj[32];
    HDC hdc;
    ULONG i;

    UNREFERENCED_PARAMETER(Param);

    while (!g_StopChurn)
    {
        /* Bitmaps and DCs are not cached by gdi32, so every call hits win32k's handle table */
        for (i = 0; i < _countof(ahObj); i++)
            ahObj[i] = CreateBitmap(1, 1, 1, 1, NULL);
        for (i = 0; i < _countof(ahObj); i++)
        {
            if (ahObj[i])
                DeleteObject(ahObj[i]);
        }

        hdc = CreateCompatibleDC(NULL);
        if (hdc)
            DeleteDC(hdc);
    }

    return 0;
}

static void
ChildMain(void)
{
    HDC hdc;
    HBITMAP hbm;

    /* Own a few objects so the exit cleanup has something to delete */
    hdc = CreateCompatibleDC(NULL);
    hbm = CreateBitmap(4, 4, 1, 1, NULL);
    if (hdc && hbm)
        ExitProcess(0x5a);
    ExitProcess(1);
}

START_TEST(GdiHandleChurn)
{
    HANDLE ahThread[CHURN_THREADS];
    CHAR szPath[MAX_PATH], szCmd[MAX_PATH + 64];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char **argv;
    ULONG i, cGood = 0, cBad = 0, cFailed = 0;
    DWORD dwExit;
    int argc;

    argc = winetest_get_mainargs(&argv);
    if (argc >= 3 && !strcmp(argv[2], "child"))
    {
        ChildMain();
        return;
    }

    GetModuleFileNameA(NULL, szPath, _countof(szPath));
    StringCbPrintfA(szCmd, sizeof(szCmd), "\"%s\" GdiHandleChurn child", szPath);

    for (i = 0; i < CHURN_THREADS; i++)
    {
        ahThread[i] = CreateThread(NULL, 0, ChurnThread, NULL, 0, NULL);
        ok(ahThread[i] != NULL, "CreateThread %lu failed: %lu\n", i, GetLastError());
    }

    for (i = 0; i < CHILD_COUNT; i++)
    {
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        if (!CreateProcessA(NULL, szCmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        {
            cFailed++;
            continue;
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        dwExit = 0;
        GetExitCodeProcess(pi.hProcess, &dwExit);
        if (dwExit == 0x5a) cGood++; else cBad++;
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }

    InterlockedExchange(&g_StopChurn, 1);
    for (i = 0; i < CHURN_THREADS; i++)
    {
        if (ahThread[i])
        {
            WaitForSingleObject(ahThread[i], INFINITE);
            CloseHandle(ahThread[i]);
        }
    }

    ok(cFailed == 0, "CreateProcess failed %lu times\n", cFailed);
    ok(cBad == 0, "%lu children exited with a wrong code\n", cBad);
    ok(cGood == CHILD_COUNT, "%lu of %u children exited cleanly\n", cGood, CHILD_COUNT);
    trace("GdiHandleChurn: %lu children exited while %u threads churned GDI objects\n",
          cGood, CHURN_THREADS);
}
