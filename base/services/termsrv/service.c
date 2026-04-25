/*
 * PROJECT:     ReactOS Terminal Services
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Minimal TermService host
 */

#include <windows.h>
#include "listener.h"
#include "termsrv.h"

#include <stdio.h>
#include <string.h>

static SERVICE_STATUS ServiceStatus;
static SERVICE_STATUS_HANDLE ServiceStatusHandle;
static HANDLE StopEvent;

static VOID
UpdateServiceStatus(
    _In_ DWORD State,
    _In_ DWORD WaitHint)
{
    static DWORD CheckPoint = 1;

    ServiceStatus.dwCurrentState = State;
    ServiceStatus.dwWin32ExitCode = NO_ERROR;
    ServiceStatus.dwWaitHint = WaitHint;
    ServiceStatus.dwControlsAccepted =
        (State == SERVICE_RUNNING) ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0;
    ServiceStatus.dwCheckPoint =
        (State == SERVICE_RUNNING || State == SERVICE_STOPPED) ? 0 : CheckPoint++;

    if (ServiceStatusHandle != NULL)
        SetServiceStatus(ServiceStatusHandle, &ServiceStatus);
}

static DWORD WINAPI
ControlHandler(
    _In_ DWORD Control,
    _In_ DWORD EventType,
    _In_opt_ LPVOID EventData,
    _In_opt_ LPVOID Context)
{
    UNREFERENCED_PARAMETER(EventType);
    UNREFERENCED_PARAMETER(EventData);
    UNREFERENCED_PARAMETER(Context);

    switch (Control)
    {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            UpdateServiceStatus(SERVICE_STOP_PENDING, 2000);
            if (StopEvent != NULL)
                SetEvent(StopEvent);
            return NO_ERROR;

        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;

        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static DWORD WINAPI
WorkerThread(
    _In_opt_ LPVOID Parameter)
{
    UNREFERENCED_PARAMETER(Parameter);

    return TermSrvListenerRun(StopEvent);
}

static VOID WINAPI
ServiceMain(
    _In_ DWORD Argc,
    _In_reads_(Argc) LPWSTR *Argv)
{
    HANDLE Worker;

    UNREFERENCED_PARAMETER(Argc);
    UNREFERENCED_PARAMETER(Argv);

    ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    ServiceStatus.dwServiceSpecificExitCode = 0;

    ServiceStatusHandle = RegisterServiceCtrlHandlerExW(TERMSRV_SERVICE_NAME_W,
                                                        ControlHandler,
                                                        NULL);
    if (ServiceStatusHandle == NULL)
        return;

    UpdateServiceStatus(SERVICE_START_PENDING, 3000);

    StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (StopEvent == NULL)
    {
        ServiceStatus.dwWin32ExitCode = GetLastError();
        UpdateServiceStatus(SERVICE_STOPPED, 0);
        return;
    }

    Worker = CreateThread(NULL, 0, WorkerThread, NULL, 0, NULL);
    if (Worker == NULL)
    {
        ServiceStatus.dwWin32ExitCode = GetLastError();
        CloseHandle(StopEvent);
        StopEvent = NULL;
        UpdateServiceStatus(SERVICE_STOPPED, 0);
        return;
    }

    UpdateServiceStatus(SERVICE_RUNNING, 0);
    WaitForSingleObject(Worker, INFINITE);

    CloseHandle(Worker);
    CloseHandle(StopEvent);
    StopEvent = NULL;
    UpdateServiceStatus(SERVICE_STOPPED, 0);
}

static INT
RunSelfTest(VOID)
{
    TERMSRV_SESSION_MANAGER SessionManager;
    TERMSRV_RDP_PEER Peer;

    TermSrvSessionManagerInit(&SessionManager);
    TermSrvRdpPeerInit(&Peer, &SessionManager);

    if (TermSrvRdpPeerReceive(&Peer, "X224 cookie=mstshash=test") != TermSrvRdpSuccess ||
        TermSrvRdpPeerReceive(&Peer, "MCS channels=rdpdr;fast=1") != TermSrvRdpSuccess ||
        TermSrvRdpPeerReceive(&Peer, "ERECT") != TermSrvRdpSuccess ||
        TermSrvRdpPeerReceive(&Peer, "ATTACH") != TermSrvRdpSuccess ||
        TermSrvRdpPeerReceive(&Peer, "JOIN id=1001") != TermSrvRdpSuccess ||
        TermSrvRdpPeerReceive(&Peer, "JOIN id=1003") != TermSrvRdpSuccess ||
        TermSrvRdpPeerReceive(&Peer, "JOIN id=1004") != TermSrvRdpSuccess ||
        TermSrvRdpPeerReceive(&Peer, "SECURITY client_random=test") != TermSrvRdpSuccess ||
        TermSrvRdpPeerReceive(&Peer, "CLIENT_INFO user=DOMAIN\\alice") != TermSrvRdpSuccess ||
        TermSrvRdpPeerReceive(&Peer, "CONFIRM_ACTIVE") != TermSrvRdpSuccess)
    {
        printf("termsrv: selftest failed in state %s\n", TermSrvRdpStateName(Peer.State));
        return 1;
    }

    printf("termsrv: selftest reached %s\n", TermSrvRdpStateName(Peer.State));
    return (Peer.State == TermSrvRdpStateActive) ? 0 : 1;
}

static INT
RunListenerConsole(VOID)
{
    DWORD Result;

    StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (StopEvent == NULL)
        return (INT)GetLastError();

    Result = TermSrvListenerRun(StopEvent);

    CloseHandle(StopEvent);
    StopEvent = NULL;
    return (INT)Result;
}

int __cdecl
wmain(
    _In_ INT Argc,
    _In_reads_(Argc) WCHAR **Argv)
{
    SERVICE_TABLE_ENTRYW Table[] =
    {
        { (LPWSTR)TERMSRV_SERVICE_NAME_W, ServiceMain },
        { NULL, NULL }
    };

    if (Argc >= 2 &&
        (_wcsicmp(Argv[1], L"-selftest") == 0 ||
         _wcsicmp(Argv[1], L"/selftest") == 0))
    {
        return RunSelfTest();
    }

    if (Argc >= 2 &&
        (_wcsicmp(Argv[1], L"-listen") == 0 ||
         _wcsicmp(Argv[1], L"/listen") == 0))
    {
        return RunListenerConsole();
    }

    return StartServiceCtrlDispatcherW(Table) ? 0 : (INT)GetLastError();
}
