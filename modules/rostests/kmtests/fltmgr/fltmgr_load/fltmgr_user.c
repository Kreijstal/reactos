/*
 * PROJECT:         ReactOS kernel-mode tests - Filter Manager
 * LICENSE:         GPLv2+ - See COPYING in the top level directory
 * PURPOSE:         Tests for checking filters load and connect correctly
 * PROGRAMMER:      Ged Murphy <gedmurphy@reactos.org>
 */

#include <kmt_test.h>


START_TEST(FltMgrLoad)
{
    static WCHAR FilterName[] = L"FltMgrLoad";
    SC_HANDLE hService;
    HANDLE hPort;
    DWORD Error;

    trace("Message from user-mode\n");

    ok(KmtFltCreateService(FilterName, L"FltMgrLoad test driver", &hService) == ERROR_SUCCESS, "\n");
    ok(KmtFltAddAltitude(L"123455") == ERROR_SUCCESS, "Failed to configure the test filter instance\n");
    ok(KmtFltLoadDriver(FALSE, FALSE, FALSE, &hPort) == ERROR_PRIVILEGE_NOT_HELD, "\n");
    ok(KmtFltLoadDriver(TRUE, FALSE, FALSE, &hPort) == ERROR_SUCCESS, "\n");

    Error = KmtFltConnectComms(&hPort);
    ok_eq_ulong(Error, ERROR_SUCCESS);

    ok(KmtFltDisconnectComms(hPort) == ERROR_SUCCESS, "\n");
    ok(KmtFltUnloadDriver(hPort, FALSE) == ERROR_SUCCESS, "\n");
    KmtFltDeleteService(NULL, &hService);
}
