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

/* libsmb2 (sync.c) poll-loop abort flag.  Set from the control handler so an
 * in-flight synchronous op bails out of wait_for_reply() within one poll tick
 * on stop/shutdown instead of pinning the single worker thread for the full
 * stall watchdog window. */
extern volatile int smb2_sync_abort;

/* No-op user APC used only to wake the worker thread out of the driver's
 * alertable wait (KeWaitForSingleObject(..., UserMode, Alertable) inside the
 * IOCTL_SMB2RDR_READ upcall fetch).  We cannot use CancelIoEx/CancelSynchronousIo
 * here because both are -stub on ReactOS. */
static VOID CALLBACK
Smb2dStopApc(ULONG_PTR param)
{
    UNREFERENCED_PARAMETER(param);
}

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
        /* Tell libsmb2's synchronous poll loop to abandon any in-flight op
         * immediately.  The worker may be parked inside wait_for_reply()
         * servicing a slow/stalled SMB op (a paging read/write has no upcall
         * timeout at all); the queued user APC below is swallowed by msafd's
         * MsafdWaitForAlert (it discards WAIT_IO_COMPLETION), so without this
         * flag the op would run to its watchdog and the process could take
         * tens of seconds to terminate on reboot. */
        smb2_sync_abort = 1;
        if (g_hStopEvent)
            SetEvent(g_hStopEvent);
        /* The worker is almost always parked in the driver's alertable wait
         * inside IOCTL_SMB2RDR_READ; SetEvent alone cannot wake it.  Queue a
         * user APC so that wait returns (STATUS_USER_APC), the IOCTL unblocks,
         * and the loop notices g_hStopEvent and exits.  Without this the
         * service hangs forever in STOP_PENDING and the process becomes
         * un-killable (the thread is stuck in an un-aborted kernel wait). */
        if (g_hWorker)
            QueueUserAPC(Smb2dStopApc, g_hWorker, 0);
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
