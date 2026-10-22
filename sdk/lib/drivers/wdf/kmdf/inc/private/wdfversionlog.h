/*
 * PROJECT:     Kernel Mode Device Framework
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Event log message identifiers of the framework (wdfversionlog.h)
 * COPYRIGHT:   2026 ReactOS contributors
 */

/*
 * Windows generates this header from the Wdf01000 message table, which is
 * not published. ReactOS has no such message table, the values only need to
 * be distinct.
 */

#pragma once

#define WDFVER_MINOR_VERSION_NOT_SUPPORTED   0x80070001
#define WDFVER_CLIENT_INVALID_DDI_COUNT      0x80070002
#define WDFVER_DRIVER_COMPANION_FAIL_TO_LOAD 0x80070003
