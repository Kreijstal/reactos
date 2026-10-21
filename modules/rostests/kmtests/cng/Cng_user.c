/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Kernel-mode test for the CNG provider (cng.sys), user-mode part
 * COPYRIGHT:   Copyright 2026 Kreijstal <rainb@tfwno.gf>
 */

#include <kmt_test.h>

#include "Cng.h"

START_TEST(Cng)
{
    DWORD Error;

    /* The test driver imports cng.sys, so loading it also loads the provider */
    Error = KmtLoadAndOpenDriver(L"Cng", FALSE);
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error)
        return;

    Error = KmtSendToDriver(IOCTL_CNG_RUN_TESTS);
    ok_eq_int(Error, ERROR_SUCCESS);

    KmtCloseDriver();
    KmtUnloadDriver();
}
