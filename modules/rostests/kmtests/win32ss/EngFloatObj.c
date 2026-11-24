/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Kernel-Mode Test Suite - x86 FLOATOBJ emulation in win32k
 *
 * On x86, win32k and display drivers do floating point arithmetic through the
 * FLOATOBJ_* exports, which emulate it with integers. The test links the
 * win32k implementation (win32ss/gdi/eng/i386/floatobj.S) and checks that
 * each export follows the stdcall convention and computes the right value.
 * The ...Float wrappers once built their temporary FLOATOBJ over the saved
 * EBP, so the first LPtoDP with a non-identity transform returned into
 * win32k with a frame pointer of 1 and bugchecked 0x50.
 */

#include <kmt_test.h>

typedef struct _KMT_FLOATOBJ
{
    ULONG ul1;
    ULONG ul2;
} KMT_FLOATOBJ, *PKMT_FLOATOBJ;

/* IEEE single precision bit patterns (FLOATL on x86) */
#define F_0_5   0x3F000000UL
#define F_1_0   0x3F800000UL
#define F_1_5   0x3FC00000UL
#define F_2_0   0x40000000UL
#define F_2_5   0x40200000UL
#define F_4_0   0x40800000UL

VOID NTAPI FLOATOBJ_SetLong(PKMT_FLOATOBJ pf, LONG l);
ULONG NTAPI FLOATOBJ_GetFloat(PKMT_FLOATOBJ pf);
VOID NTAPI FLOATOBJ_AddFloat(PKMT_FLOATOBJ pf, ULONG f);
VOID NTAPI FLOATOBJ_SubFloat(PKMT_FLOATOBJ pf, ULONG f);
VOID NTAPI FLOATOBJ_MulFloat(PKMT_FLOATOBJ pf, ULONG f);
VOID NTAPI FLOATOBJ_DivFloat(PKMT_FLOATOBJ pf, ULONG f);
VOID NTAPI FLOATOBJ_AddLong(PKMT_FLOATOBJ pf, LONG l);
VOID NTAPI FLOATOBJ_SubLong(PKMT_FLOATOBJ pf, LONG l);
VOID NTAPI FLOATOBJ_MulLong(PKMT_FLOATOBJ pf, LONG l);
VOID NTAPI FLOATOBJ_DivLong(PKMT_FLOATOBJ pf, LONG l);
ULONG NTAPI FLOATOBJ_GreaterThanLong(PKMT_FLOATOBJ pf, LONG l);
ULONG NTAPI FLOATOBJ_LessThanLong(PKMT_FLOATOBJ pf, LONG l);
ULONG NTAPI FLOATOBJ_EqualLong(PKMT_FLOATOBJ pf, LONG l);

BOOLEAN NTAPI KmtCallPreservesFramePointer(PVOID Function, PVOID Argument1, ULONG Argument2);

static const struct
{
    PCSTR Name;
    PVOID Function;
    ULONG Argument;
    ULONG Expected;     /* 2 op Argument, as FLOATL */
} FloatObjTests[] =
{
    { "AddFloat",  FLOATOBJ_AddFloat, F_0_5, F_2_5 },
    { "SubFloat",  FLOATOBJ_SubFloat, F_0_5, F_1_5 },
    { "MulFloat",  FLOATOBJ_MulFloat, F_0_5, F_1_0 },
    { "DivFloat",  FLOATOBJ_DivFloat, F_0_5, F_4_0 },
    { "AddLong",   FLOATOBJ_AddLong,  2,     F_4_0 },
    { "SubLong",   FLOATOBJ_SubLong,  1,     F_1_0 },
    { "MulLong",   FLOATOBJ_MulLong,  2,     F_4_0 },
    { "DivLong",   FLOATOBJ_DivLong,  2,     F_1_0 },
};

START_TEST(EngFloatObj)
{
    KMT_FLOATOBJ Value;
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(FloatObjTests); i++)
    {
        FLOATOBJ_SetLong(&Value, 2);
        ok(KmtCallPreservesFramePointer(FloatObjTests[i].Function, &Value, FloatObjTests[i].Argument),
           "FLOATOBJ_%s does not preserve EBP\n", FloatObjTests[i].Name);
        ok_eq_hex(FLOATOBJ_GetFloat(&Value), FloatObjTests[i].Expected);
    }

    FLOATOBJ_SetLong(&Value, 2);
    ok(KmtCallPreservesFramePointer(FLOATOBJ_GreaterThanLong, &Value, 1),
       "FLOATOBJ_GreaterThanLong does not preserve EBP\n");
    ok(KmtCallPreservesFramePointer(FLOATOBJ_LessThanLong, &Value, 3),
       "FLOATOBJ_LessThanLong does not preserve EBP\n");
    ok(KmtCallPreservesFramePointer(FLOATOBJ_EqualLong, &Value, 2),
       "FLOATOBJ_EqualLong does not preserve EBP\n");
    ok(FLOATOBJ_GreaterThanLong(&Value, 1), "2 > 1 is FALSE\n");
    ok(!FLOATOBJ_GreaterThanLong(&Value, 2), "2 > 2 is TRUE\n");
    ok(FLOATOBJ_LessThanLong(&Value, 3), "2 < 3 is FALSE\n");
    ok(!FLOATOBJ_LessThanLong(&Value, 2), "2 < 2 is TRUE\n");
    ok(FLOATOBJ_EqualLong(&Value, 2), "2 == 2 is FALSE\n");
    ok_eq_hex(FLOATOBJ_GetFloat(&Value), F_2_0);
}
