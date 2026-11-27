/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Test the lifetime of the WLAN service process
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 *
 * A service that reports SERVICE_RUNNING takes calls, so a client started
 * right after it does not fail; and the process of an own-process service
 * ends when the service stops.  Restarts WlanSvc, so it needs the rights
 * to; skipped otherwise.
 */

#include <apitest.h>
#include <wlanapi.h>
#include <winsvc.h>

/* Poll without sleeping between queries: the first RUNNING counts. */
static BOOL
WaitState(SC_HANDLE Service, DWORD State, SERVICE_STATUS_PROCESS *Status)
{
    DWORD Needed, Start = GetTickCount();

    do
    {
        if (!QueryServiceStatusEx(Service, SC_STATUS_PROCESS_INFO, (LPBYTE)Status,
                                  sizeof(*Status), &Needed))
        {
            return FALSE;
        }
        if (Status->dwCurrentState == State)
            return TRUE;
        Sleep(0);
    } while (GetTickCount() - Start < 30000);
    return FALSE;
}

START_TEST(svclife)
{
    SC_HANDLE Manager, Service;
    SERVICE_STATUS_PROCESS Status;
    SERVICE_STATUS Control;
    HANDLE Process, Client;
    DWORD Negotiated, Ret, Pid, Wait;
    ULONG Round;

    Manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (Manager == NULL)
    {
        skip("No service manager (%lu)\n", GetLastError());
        return;
    }
    Service = OpenServiceW(Manager, L"WlanSvc",
                           SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS);
    if (Service == NULL)
    {
        skip("Cannot open WlanSvc (%lu)\n", GetLastError());
        CloseServiceHandle(Manager);
        return;
    }
    if (!WaitState(Service, SERVICE_RUNNING, &Status))
    {
        skip("WlanSvc is not running\n");
        goto done;
    }

    for (Round = 0; Round < 3; Round++)
    {
        Pid = Status.dwProcessId;
        Process = OpenProcess(SYNCHRONIZE, FALSE, Pid);
        ok(Process != NULL, "Cannot open the process %lu (%lu)\n", Pid, GetLastError());

        if (!ControlService(Service, SERVICE_CONTROL_STOP, &Control))
        {
            skip("Cannot stop WlanSvc (%lu)\n", GetLastError());
            if (Process != NULL)
                CloseHandle(Process);
            goto done;
        }
        ok(WaitState(Service, SERVICE_STOPPED, &Status), "WlanSvc did not stop\n");

        /* The service is gone, and its process with it. */
        if (Process != NULL)
        {
            Wait = WaitForSingleObject(Process, 10000);
            ok(Wait == WAIT_OBJECT_0,
               "Round %lu: wlansvc process %lu still runs after the service stopped (%lu)\n",
               Round, Pid, Wait);
            CloseHandle(Process);
        }

        if (!StartServiceW(Service, 0, NULL))
        {
            ok(FALSE, "Cannot start WlanSvc again (%lu)\n", GetLastError());
            goto done;
        }
        if (!WaitState(Service, SERVICE_RUNNING, &Status))
        {
            ok(FALSE, "WlanSvc did not come back\n");
            goto done;
        }

        /* Running means taking calls: no retry. */
        Ret = WlanOpenHandle(2, NULL, &Negotiated, &Client);
        ok(Ret == ERROR_SUCCESS,
           "Round %lu: WlanOpenHandle right after SERVICE_RUNNING returned %lu\n",
           Round, Ret);
        if (Ret == ERROR_SUCCESS)
            WlanCloseHandle(Client, NULL);
        ok(Status.dwProcessId != Pid, "Round %lu: same process %lu\n", Round, Pid);
    }

done:
    CloseServiceHandle(Service);
    CloseServiceHandle(Manager);
}
