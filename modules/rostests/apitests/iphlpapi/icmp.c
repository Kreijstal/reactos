/*
 * PROJECT:         ReactOS API Tests
 * LICENSE:         LGPLv2.1+ - See COPYING.LIB in the top level directory
 * PURPOSE:         Tests for ICMP functions
 * PROGRAMMERS:     Tim Crawford
 */

#include <apitest.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>

static
void
test_IcmpCreateFile(void)
{
    HANDLE hIcmp;

    SetLastError(0xDEADBEEF);
    hIcmp = IcmpCreateFile();
    ok(hIcmp != INVALID_HANDLE_VALUE, "IcmpCreateFile failed unexpectedly: %lu\n", GetLastError());

    if (hIcmp != INVALID_HANDLE_VALUE)
        IcmpCloseHandle(hIcmp);
}

static
void
test_Icmp6CreateFile(void)
{
    HANDLE hIcmp;

    SetLastError(0xDEADBEEF);
    hIcmp = Icmp6CreateFile();

    if (GetLastError() == ERROR_FILE_NOT_FOUND)
    {
        /* On Windows Server 2003, the IPv6 protocol must be installed. */
        skip("IPv6 is not available.\n");
        return;
    }

    ok(hIcmp != INVALID_HANDLE_VALUE, "Icmp6CreateFile failed unexpectedly: %lu\n", GetLastError());

    if (hIcmp != INVALID_HANDLE_VALUE)
        IcmpCloseHandle(hIcmp);
}

static
void
test_IcmpCloseHandle(void)
{
    HANDLE hIcmp;
    BOOL bRet;
    DWORD LastError;

    SetLastError(0xDEADBEEF);
    hIcmp = IcmpCreateFile();
    if (hIcmp != INVALID_HANDLE_VALUE)
    {
        bRet = IcmpCloseHandle(hIcmp);
        ok(bRet, "IcmpCloseHandle failed unexpectedly: %lu\n", GetLastError());
    }

    SetLastError(0xDEADBEEF);
    hIcmp = Icmp6CreateFile();
    if (hIcmp != INVALID_HANDLE_VALUE)
    {
        bRet = IcmpCloseHandle(hIcmp);
        ok(bRet, "IcmpCloseHandle failed unexpectedly: %lu\n", GetLastError());
    }

    hIcmp = INVALID_HANDLE_VALUE;
    SetLastError(0xDEADBEEF);
    bRet = IcmpCloseHandle(hIcmp);
    ok(!bRet, "IcmpCloseHandle succeeded unexpectedly\n");
    LastError = GetLastError();
    ok(LastError == ERROR_INVALID_HANDLE || LastError == ERROR_INVALID_PARAMETER,
       "Unexpected last error (0x%lX)\n", LastError);

    hIcmp = NULL;
    SetLastError(0xDEADBEEF);
    bRet = IcmpCloseHandle(hIcmp);
    ok(!bRet, "IcmpCloseHandle succeeded unexpectedly\n");
    LastError = GetLastError();
    ok(LastError == ERROR_INVALID_HANDLE || LastError == ERROR_INVALID_PARAMETER,
       "Unexpected last error (0x%lX)\n", LastError);
}

static
void
test_IcmpSendEcho(void)
{
    HANDLE hIcmp;
    unsigned long ipaddr = INADDR_NONE;
    DWORD bRet = 0, error = 0;
    char SendData[32] = "Data Buffer";
    PVOID ReplyBuffer;
    DWORD ReplySize = 0;

    SetLastError(0xDEADBEEF);
    hIcmp = IcmpCreateFile();
    if (hIcmp == INVALID_HANDLE_VALUE)
    {
        skip("IcmpCreateFile failed unexpectedly: %lu\n", GetLastError());
        return;
    }

    ipaddr = 0x08080808; // 8.8.8.8
    ReplyBuffer = malloc(sizeof(ICMP_ECHO_REPLY) + sizeof(SendData));

    ReplySize = sizeof(ICMP_ECHO_REPLY);
    SetLastError(0xDEADBEEF);
    bRet = IcmpSendEcho(hIcmp, ipaddr, SendData, sizeof(SendData),
        NULL, ReplyBuffer, ReplySize, 5000);

    ok(!bRet, "IcmpSendEcho succeeded unexpectedly\n");
    error = GetLastError();
    ok(error == IP_BUF_TOO_SMALL /* Win2003 */ ||
       error == IP_GENERAL_FAILURE /* Win10 */,
       "IcmpSendEcho returned unexpected error: %lu\n", error);

    ReplySize = sizeof(ICMP_ECHO_REPLY) + sizeof(SendData);
    SetLastError(0xDEADBEEF);
    bRet = IcmpSendEcho(hIcmp, ipaddr, SendData, sizeof(SendData),
        NULL, ReplyBuffer, ReplySize, 5000);

    ok(bRet, "IcmpSendEcho failed unexpectedly: %lu\n", GetLastError());

    free(ReplyBuffer);
    IcmpCloseHandle(hIcmp);
}

typedef DWORD (WINAPI *PFN_ICMPSENDECHO2EX)(HANDLE, HANDLE, PVOID, PVOID, IPAddr, IPAddr,
                                            LPVOID, WORD, PIP_OPTION_INFORMATION,
                                            LPVOID, DWORD, DWORD);

/* The first adapter with an IPv4 address and a gateway */
static
BOOL
GetLocalAndGateway(IPAddr *Local, IPAddr *Gateway)
{
    ULONG Size = 0;
    PIP_ADAPTER_INFO Info, Adapter;
    BOOL Found = FALSE;

    if (GetAdaptersInfo(NULL, &Size) != ERROR_BUFFER_OVERFLOW)
        return FALSE;
    Info = malloc(Size);
    if (!Info)
        return FALSE;
    if (GetAdaptersInfo(Info, &Size) == ERROR_SUCCESS)
    {
        for (Adapter = Info; Adapter && !Found; Adapter = Adapter->Next)
        {
            *Local = inet_addr(Adapter->IpAddressList.IpAddress.String);
            *Gateway = inet_addr(Adapter->GatewayList.IpAddress.String);
            Found = (*Local != 0 && *Local != INADDR_NONE &&
                     *Gateway != 0 && *Gateway != INADDR_NONE);
        }
    }
    free(Info);
    return Found;
}

static
void
test_IcmpSendEcho2Ex(void)
{
    PFN_ICMPSENDECHO2EX pIcmpSendEcho2Ex;
    HANDLE hIcmp;
    char SendData[32] = "Data Buffer";
    DWORD ReplySize = sizeof(ICMP_ECHO_REPLY) + sizeof(SendData) + 8 + 2 * sizeof(PVOID); /* + IO_STATUS_BLOCK */
    PICMP_ECHO_REPLY Reply;
    IPAddr Loopback = htonl(INADDR_LOOPBACK), Local, Gateway;
    DWORD nReplies;

    pIcmpSendEcho2Ex = (PFN_ICMPSENDECHO2EX)GetProcAddress(GetModuleHandleW(L"iphlpapi.dll"), "IcmpSendEcho2Ex");
    ok(pIcmpSendEcho2Ex != NULL, "IcmpSendEcho2Ex is not exported\n");
    if (!pIcmpSendEcho2Ex)
        return;

    hIcmp = IcmpCreateFile();
    if (hIcmp == INVALID_HANDLE_VALUE)
    {
        skip("IcmpCreateFile failed unexpectedly: %lu\n", GetLastError());
        return;
    }
    Reply = malloc(ReplySize);

    /* Loopback from the loopback address */
    SetLastError(0xDEADBEEF);
    nReplies = pIcmpSendEcho2Ex(hIcmp, NULL, NULL, NULL, Loopback, Loopback,
                                SendData, sizeof(SendData), NULL, Reply, ReplySize, 5000);
    ok(nReplies == 1, "loopback: %lu replies, error %lu\n", nReplies, GetLastError());
    if (nReplies == 1)
    {
        ok(Reply->Status == IP_SUCCESS, "loopback: status %lu\n", Reply->Status);
        ok(Reply->Address == Loopback, "loopback: reply from 0x%lx\n", Reply->Address);
    }

    /* A source address that is not ours is refused */
    SetLastError(0xDEADBEEF);
    nReplies = pIcmpSendEcho2Ex(hIcmp, NULL, NULL, NULL, inet_addr("192.0.2.77"), Loopback,
                                SendData, sizeof(SendData), NULL, Reply, ReplySize, 2000);
    ok(nReplies == 0, "foreign source: %lu replies\n", nReplies);

    /* From the adapter's own address to its gateway: the echo leaves through that adapter */
    if (GetLocalAndGateway(&Local, &Gateway))
    {
        SetLastError(0xDEADBEEF);
        nReplies = pIcmpSendEcho2Ex(hIcmp, NULL, NULL, NULL, Local, Gateway,
                                    SendData, sizeof(SendData), NULL, Reply, ReplySize, 5000);
        ok(nReplies == 1, "adapter source: %lu replies, error %lu\n", nReplies, GetLastError());
        if (nReplies == 1)
        {
            ok(Reply->Status == IP_SUCCESS, "adapter source: status %lu\n", Reply->Status);
            ok(Reply->Address == Gateway, "adapter source: reply from 0x%lx\n", Reply->Address);
        }
    }
    else
    {
        skip("No adapter with an IPv4 address and a gateway\n");
    }

    free(Reply);
    IcmpCloseHandle(hIcmp);
}

START_TEST(icmp)
{
    test_IcmpCreateFile();
    test_Icmp6CreateFile();
    test_IcmpCloseHandle();
    test_IcmpSendEcho();
    test_IcmpSendEcho2Ex();
}
