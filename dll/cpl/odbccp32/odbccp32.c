/*
 * PROJECT:         ReactOS ODBC Control Panel Applet
 * FILE:            dll/cpl/odbccp32/odbccp32.c
 * PURPOSE:         applet initialization
 * PROGRAMMER:      Johannes Anderwald
 */

#include "odbccp32.h"
#include "resource.h"

HINSTANCE hApplet = NULL;
APPLET_PROC ODBCProc = NULL;
HMODULE hLibrary = NULL;


/*
 * This applet is only a shim: all of the work is done by ODBCCPlApplet(),
 * which is exported by odbccp32.dll. Load it on demand.
 */
static APPLET_PROC
GetODBCAppletProc(VOID)
{
    TCHAR szBuffer[MAX_PATH];

    if (ODBCProc != NULL)
        return ODBCProc;

    if (hLibrary == NULL)
    {
        if (ExpandEnvironmentStrings(_T("%systemroot%\\system32\\odbccp32.dll"),
                                     szBuffer,
                                     sizeof(szBuffer) / sizeof(TCHAR)) == 0)
        {
            return NULL;
        }

        hLibrary = LoadLibrary(szBuffer);
        if (hLibrary == NULL)
            return NULL;
    }

    ODBCProc = (APPLET_PROC)GetProcAddress(hLibrary, "ODBCCPlApplet");
    if (ODBCProc == NULL)
    {
        FreeLibrary(hLibrary);
        hLibrary = NULL;
    }

    return ODBCProc;
}

static VOID
ReleaseODBCAppletProc(VOID)
{
    ODBCProc = NULL;

    if (hLibrary != NULL)
    {
        FreeLibrary(hLibrary);
        hLibrary = NULL;
    }
}


LONG
CALLBACK
CPlApplet(HWND hwndCpl,
          UINT uMsg,
          LPARAM lParam1,
          LPARAM lParam2)
{
    switch (uMsg)
    {
        case CPL_INIT:
            return TRUE;

        case CPL_GETCOUNT:
            return 1;

        case CPL_INQUIRE:
        {
            CPLINFO *pCplInfo = (CPLINFO *)lParam2;

            pCplInfo->lData = 0;
            pCplInfo->idIcon = IDI_CPLICON;
            pCplInfo->idName = IDS_CPLNAME;
            pCplInfo->idInfo = IDS_CPLDESCRIPTION;
            return 0;
        }

        case CPL_DBLCLK:
        {
            APPLET_PROC Proc = GetODBCAppletProc();

            if (Proc == NULL)
                return (LONG)-1;

            return Proc(hwndCpl, uMsg, lParam1, lParam2);
        }

        case CPL_STOP:
            break;

        case CPL_EXIT:
            ReleaseODBCAppletProc();
            break;
    }

    return FALSE;
}


BOOL
WINAPI
DllMain(HINSTANCE hinstDLL,
        DWORD dwReason,
        LPVOID lpReserved)
{
    INITCOMMONCONTROLSEX InitControls;
    UNREFERENCED_PARAMETER(lpReserved);

    switch(dwReason)
    {
        case DLL_PROCESS_ATTACH:
        {
            InitControls.dwSize = sizeof(INITCOMMONCONTROLSEX);
            InitControls.dwICC = ICC_LISTVIEW_CLASSES | ICC_UPDOWN_CLASS | ICC_BAR_CLASSES;
            InitCommonControlsEx(&InitControls);

            hApplet = hinstDLL;
            DisableThreadLibraryCalls(hinstDLL);
            break;
        }
    }

    return TRUE;
}
