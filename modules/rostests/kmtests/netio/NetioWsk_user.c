/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel-mode test for the WSK provider in netio.sys, user-mode part
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <kmt_test.h>

#include "NetioWsk.h"

START_TEST(NetioWsk)
{
    DWORD Error;

    /* The test driver imports netio.sys, so loading it also loads the provider */
    Error = KmtLoadAndOpenDriver(L"NetioWsk", FALSE);
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error)
        return;

    Error = KmtSendToDriver(IOCTL_NETIO_WSK_RUN_TESTS);
    ok_eq_int(Error, ERROR_SUCCESS);

    KmtCloseDriver();
    KmtUnloadDriver();
}
