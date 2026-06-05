/*
 * PROJECT:     ReactOS CRT library
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     ARM64 implementation of _fpreset
 */

#include "fpcr.h"

void _fpreset(void)
{
    __setfpcr(0);
    __setfpsr(0);
}
