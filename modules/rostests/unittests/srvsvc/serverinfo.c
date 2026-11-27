/*
 * PROJECT:     ReactOS Server Service
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Unit tests for the NetrServerGetInfo server routine
 */

#include "precomp.h"
#include <lmerr.h>
#include <apitest.h>

/* Normally owned by srvsvc.c; the Browser sets SV_TYPE_POTENTIAL_BROWSER */
DWORD dwServiceBits = SV_TYPE_WORKSTATION | SV_TYPE_SERVER | SV_TYPE_POTENTIAL_BROWSER;

/*
 * The server stub frees the returned union and then every embedded pointer
 * with midl_user_free, one by one.  So every pointer handed back must be the
 * start of its own heap block, or the stub frees a pointer into the middle
 * of a block and corrupts the server process heap.
 */
static VOID
FreeLikeTheStub(PVOID Block, LPWSTR Name)
{
    SIZE_T BlockSize;
    BOOL bOwnBlock;

    BlockSize = HeapSize(GetProcessHeap(), 0, Block);
    ok(BlockSize != (SIZE_T)-1, "info %p is not a heap block\n", Block);

    bOwnBlock = ((PBYTE)Name < (PBYTE)Block || (PBYTE)Name >= (PBYTE)Block + BlockSize) &&
                HeapValidate(GetProcessHeap(), 0, Name);
    ok(bOwnBlock, "name %p is not a heap block of its own (info %p, %Iu bytes)\n",
       Name, Block, BlockSize);

    /* Free it only when it is a block: freeing an interior pointer is the bug */
    if (bOwnBlock)
        midl_user_free(Name);
    midl_user_free(Block);

    ok(HeapValidate(GetProcessHeap(), 0, NULL), "process heap is corrupt\n");
}

START_TEST(ServerInfo)
{
    WCHAR szComputerName[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD dwLength = ARRAYSIZE(szComputerName);
    LPSERVER_INFO pInfo;
    NET_API_STATUS Status;
    ULONG i;

    ok(GetComputerNameW(szComputerName, &dwLength), "GetComputerNameW failed %lu\n", GetLastError());

    for (i = 0; i < 3; i++)
    {
        pInfo = NULL;
        Status = NetrServerGetInfo(NULL, 100, &pInfo);
        ok(Status == NERR_Success, "level 100: status %lu\n", Status);
        if (Status != NERR_Success || pInfo == NULL)
            break;
        ok(pInfo->ServerInfo100.sv100_platform_id == PLATFORM_ID_NT, "level 100: platform %lu\n",
           pInfo->ServerInfo100.sv100_platform_id);
        ok(!wcscmp(pInfo->ServerInfo100.sv100_name, szComputerName), "level 100: name %ls\n",
           pInfo->ServerInfo100.sv100_name);
        FreeLikeTheStub(pInfo, pInfo->ServerInfo100.sv100_name);

        pInfo = NULL;
        Status = NetrServerGetInfo(NULL, 101, &pInfo);
        ok(Status == NERR_Success, "level 101: status %lu\n", Status);
        if (Status != NERR_Success || pInfo == NULL)
            break;
        ok(!wcscmp(pInfo->ServerInfo101.sv101_name, szComputerName), "level 101: name %ls\n",
           pInfo->ServerInfo101.sv101_name);
        ok(pInfo->ServerInfo101.sv101_type == (dwServiceBits | SV_TYPE_NT), "level 101: type 0x%lx\n",
           pInfo->ServerInfo101.sv101_type);
        ok(pInfo->ServerInfo101.sv101_comment == NULL, "level 101: comment %p\n",
           pInfo->ServerInfo101.sv101_comment);
        FreeLikeTheStub(pInfo, pInfo->ServerInfo101.sv101_name);

        pInfo = NULL;
        Status = NetrServerGetInfo(NULL, 102, &pInfo);
        ok(Status == NERR_Success, "level 102: status %lu\n", Status);
        if (Status != NERR_Success || pInfo == NULL)
            break;
        ok(!wcscmp(pInfo->ServerInfo102.sv102_name, szComputerName), "level 102: name %ls\n",
           pInfo->ServerInfo102.sv102_name);
        ok(pInfo->ServerInfo102.sv102_comment == NULL, "level 102: comment %p\n",
           pInfo->ServerInfo102.sv102_comment);
        ok(pInfo->ServerInfo102.sv102_userpath == NULL, "level 102: userpath %p\n",
           pInfo->ServerInfo102.sv102_userpath);
        FreeLikeTheStub(pInfo, pInfo->ServerInfo102.sv102_name);
    }

    pInfo = NULL;
    Status = NetrServerGetInfo(NULL, 103, &pInfo);
    ok(Status == ERROR_INVALID_LEVEL, "level 103: status %lu\n", Status);
    ok(pInfo == NULL, "level 103: info %p\n", pInfo);
}
