/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite device idle detection test user-mode part
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

#include <kmt_test.h>
#include "PoIdle.h"

START_TEST(PoIdle)
{
    DWORD Error;

    Error = KmtLoadAndOpenDriver(L"PoIdle", FALSE);
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error)
        return;

    KmtSendToDriver(IOCTL_RUN_TEST);
    KmtCloseDriver();
    KmtUnloadDriver();
}
