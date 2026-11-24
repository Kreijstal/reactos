/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Kernel-Mode Test Suite battery class and power source test user-mode part
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

#include <kmt_test.h>
#include "PoBattery.h"

START_TEST(PoBattery)
{
    DWORD Error;

    Error = KmtLoadAndOpenDriver(L"PoBattery", FALSE);
    ok_eq_int(Error, ERROR_SUCCESS);
    if (Error)
        return;

    KmtSendToDriver(IOCTL_TEST_BATTERY_WAITS);
    KmtSendToDriver(IOCTL_TEST_POWER_SOURCE);
    KmtCloseDriver();
    KmtUnloadDriver();
}
