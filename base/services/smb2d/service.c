/*
 * smb2d - usermode SMB2 daemon service.  This file hosts the SCM plumbing
 * (ServiceMain / control handler) and spawns a worker thread that runs
 * the upcall loop in daemon.c.  The daemon itself owns all interaction
 * with the smb2rdr driver; service.c just keeps SCM informed.
 */

#include <windows.h>
#include <stdio.h>

#include "smb2d.h"

static SERVICE_STATUS        g_ssStatus;
static SERVICE_STATUS_HANDLE g_ssHandle;
static HANDLE                g_hStopEvent;
static HANDLE                g_hWorker;

static VOID
UpdateStatus(DWORD state, DWORD waitHint)
{
    static DWORD checkpoint = 1;

    g_ssStatus.dwCurrentState = state;
    g_ssStatus.dwWin32ExitCode = NO_ERROR;
    g_ssStatus.dwWaitHint = waitHint;

    if (state == SERVICE_START_PENDING)
        g_ssStatus.dwControlsAccepted = 0;
    else
        g_ssStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;

    if (state == SERVICE_RUNNING || state == SERVICE_STOPPED)
        g_ssStatus.dwCheckPoint = 0;
    else
        g_ssStatus.dwCheckPoint = checkpoint++;

    SetServiceStatus(g_ssHandle, &g_ssStatus);
}

static DWORD WINAPI
ControlHandler(DWORD ctrl, DWORD eventType, LPVOID eventData, LPVOID ctx)
{
    UNREFERENCED_PARAMETER(eventType);
    UNREFERENCED_PARAMETER(eventData);
    UNREFERENCED_PARAMETER(ctx);

    switch (ctrl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        UpdateStatus(SERVICE_STOP_PENDING, 2000);
        if (g_hStopEvent)
            SetEvent(g_hStopEvent);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static DWORD WINAPI
WorkerThread(LPVOID param)
{
    UNREFERENCED_PARAMETER(param);
    return Smb2dDaemonLoop(g_hStopEvent);
}

static VOID WINAPI
ServiceMain(DWORD argc, LPWSTR *argv)
{
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    g_ssStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ssStatus.dwServiceSpecificExitCode = 0;

    g_ssHandle = RegisterServiceCtrlHandlerExW(SMB2D_SERVICE_NAME_W,
                                               ControlHandler, NULL);
    if (!g_ssHandle)
        return;

    UpdateStatus(SERVICE_START_PENDING, 3000);

    g_hStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_hStopEvent) {
        g_ssStatus.dwWin32ExitCode = GetLastError();
        UpdateStatus(SERVICE_STOPPED, 0);
        return;
    }

    g_hWorker = CreateThread(NULL, 0, WorkerThread, NULL, 0, NULL);
    if (!g_hWorker) {
        g_ssStatus.dwWin32ExitCode = GetLastError();
        CloseHandle(g_hStopEvent);
        g_hStopEvent = NULL;
        UpdateStatus(SERVICE_STOPPED, 0);
        return;
    }

    UpdateStatus(SERVICE_RUNNING, 0);

    WaitForSingleObject(g_hWorker, INFINITE);
    CloseHandle(g_hWorker);
    g_hWorker = NULL;
    CloseHandle(g_hStopEvent);
    g_hStopEvent = NULL;

    UpdateStatus(SERVICE_STOPPED, 0);
}

int __cdecl
main(int argc, char **argv)
{
    SERVICE_TABLE_ENTRYW table[] = {
        { (LPWSTR)SMB2D_SERVICE_NAME_W, ServiceMain },
        { NULL, NULL }
    };

    /* Allow a one-shot debug invocation from a console by passing -debug,
     * mostly so we can smoke-test the upcall loop from a shell before SCM
     * is stable.  Otherwise hand control to the SCM. */
    if (argc >= 2 && (_stricmp(argv[1], "-debug") == 0 ||
                      _stricmp(argv[1], "/debug") == 0)) {
        HANDLE h = CreateEventW(NULL, TRUE, FALSE, NULL);
        printf("smb2d: running in -debug mode, Ctrl-C to stop\n");
        Smb2dDaemonLoop(h);
        CloseHandle(h);
        return 0;
    }

    /* Self-test mode: spawn the daemon loop in a background thread, then
     * issue a CreateFile on a hard-coded UNC probe path to drive a
     * CreateSrvCall upcall through the bridge.  Useful for smoke-testing
     * the kernel/user plumbing end-to-end without depending on the SCM.
     * Second arg is the UNC path; defaults to
     * \\10.0.2.2\public\smb2probe.txt.  Returns once the upcall has been
     * round-tripped (or after a short overall timeout). */
    if (argc >= 2 && (_stricmp(argv[1], "-selftest") == 0 ||
                      _stricmp(argv[1], "/selftest") == 0)) {
        const char *target = (argc >= 3) ? argv[2]
                                         : "\\\\10.0.2.2\\public\\smb2probe.txt";
        HANDLE worker;
        HANDLE h;

        printf("smb2d: selftest starting; daemon thread + UNC probe on %s\n",
               target);
        fflush(stdout);

        /* Use the global stop event so the worker loop has something to
         * wake on when the selftest is done (ignored path today). */
        g_hStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        worker = CreateThread(NULL, 0, WorkerThread, NULL, 0, NULL);
        if (!worker) {
            printf("smb2d: selftest failed to spawn worker: %lu\n",
                   GetLastError());
            return 2;
        }

        /* Give the daemon loop a beat to open \\.\smb2rdr and block on
         * IOCTL_SMB2RDR_READ before we kick the UNC probe. */
        Sleep(500);

        printf("smb2d: selftest CreateFileA(%s)\n", target);
        fflush(stdout);
        h = CreateFileA(target,
                        GENERIC_READ,
                        FILE_SHARE_READ,
                        NULL,
                        OPEN_EXISTING,
                        0,
                        NULL);
        printf("smb2d: selftest CreateFile -> handle=%p err=%lu\n",
               h, GetLastError());
        fflush(stdout);
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);

        /* Let the worker drain one more upcall in case retries come in. */
        Sleep(3000);
        return 0;
    }

    if (!StartServiceCtrlDispatcherW(table)) {
        fprintf(stderr, "smb2d: StartServiceCtrlDispatcherW failed: %lu\n",
                GetLastError());
        return 1;
    }
    return 0;
}
