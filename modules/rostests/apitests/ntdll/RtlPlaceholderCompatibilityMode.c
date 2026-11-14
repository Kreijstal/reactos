/*
 * PROJECT:     ReactOS api tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Test for the process placeholder compatibility mode
 * COPYRIGHT:   Copyright 2026 ReactOS Team
 */

#include "precomp.h"

#define PHCM_APPLICATION_DEFAULT        0
#define PHCM_DISGUISE_PLACEHOLDER       1
#define PHCM_EXPOSE_PLACEHOLDERS        2
#define PHCM_DISGUISE_FULL_PLACEHOLDERS 3
#define PHCM_ERROR_INVALID_PARAMETER    (-1)

typedef CHAR (NTAPI *FN_RtlQueryProcessPlaceholderCompatibilityMode)(VOID);
typedef CHAR (NTAPI *FN_RtlSetProcessPlaceholderCompatibilityMode)(CHAR);

START_TEST(RtlPlaceholderCompatibilityMode)
{
    HMODULE Ntdll = GetModuleHandleW(L"ntdll.dll");
    FN_RtlQueryProcessPlaceholderCompatibilityMode pQuery;
    FN_RtlSetProcessPlaceholderCompatibilityMode pSet;
    CHAR Initial;

    pQuery = (FN_RtlQueryProcessPlaceholderCompatibilityMode)
        GetProcAddress(Ntdll, "RtlQueryProcessPlaceholderCompatibilityMode");
    pSet = (FN_RtlSetProcessPlaceholderCompatibilityMode)
        GetProcAddress(Ntdll, "RtlSetProcessPlaceholderCompatibilityMode");
    if (!pQuery || !pSet)
    {
        ok(GetNTVersion() < _WIN32_WINNT_WIN10, "Placeholder compatibility functions are not exported\n");
        return;
    }

    Initial = pQuery();
    ok(Initial >= PHCM_APPLICATION_DEFAULT && Initial <= PHCM_DISGUISE_FULL_PLACEHOLDERS,
       "Initial mode %d\n", Initial);

    /* Setting returns the previous mode */
    ok_int(pSet(PHCM_EXPOSE_PLACEHOLDERS), Initial);
    ok_int(pQuery(), PHCM_EXPOSE_PLACEHOLDERS);
    ok_int(pSet(PHCM_DISGUISE_PLACEHOLDER), PHCM_EXPOSE_PLACEHOLDERS);
    ok_int(pQuery(), PHCM_DISGUISE_PLACEHOLDER);

    /* Unknown modes are rejected and leave the mode alone */
    ok_int(pSet(4), PHCM_ERROR_INVALID_PARAMETER);
    ok_int(pSet(-3), PHCM_ERROR_INVALID_PARAMETER);
    ok_int(pQuery(), PHCM_DISGUISE_PLACEHOLDER);

    ok_int(pSet(Initial), PHCM_DISGUISE_PLACEHOLDER);
    ok_int(pQuery(), Initial);
}
