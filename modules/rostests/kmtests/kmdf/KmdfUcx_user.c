/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     UCX class extension binding test, user-mode part
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 */

#include <kmt_test.h>

#include "../kmtest/kmtest.h"
#include "KmdfUcx.h"

START_TEST(KmdfUcx)
{
    SC_HANDLE UcxService = NULL;
    DWORD Error;

    Error = KmtCreateService(UCX_SERVICE_NAME, UCX_IMAGE_NAME, NULL, &UcxService);
    if (Error == ERROR_SERVICE_EXISTS)
        Error = ERROR_SUCCESS;
    ok_eq_int(Error, ERROR_SUCCESS);

    Error = KmtLoadAndOpenDriver(L"KmdfUcx", FALSE);
    ok_eq_int(Error, ERROR_SUCCESS);
    if (!Error)
    {
        Error = KmtSendToDriver(IOCTL_KMDFUCX_RUN_TESTS);
        ok_eq_int(Error, ERROR_SUCCESS);

        KmtCloseDriver();
        KmtUnloadDriver();
    }

    if (UcxService)
        KmtDeleteService(UCX_SERVICE_NAME, &UcxService);
}
