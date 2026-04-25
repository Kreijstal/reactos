/*
 * PROJECT:     ReactOS Terminal Services
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Disabled-by-default TCP listener scaffold
 */

#pragma once

#include <windows.h>

DWORD
TermSrvListenerRun(
    _In_ HANDLE StopEvent);
