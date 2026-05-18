/*
 * PROJECT:     ReactOS DirectWrite stub
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     Minimal DWrite.dll surface so modern installers load.
 */

#include <stdarg.h>

#include <windef.h>
#include <winbase.h>
#include <objbase.h>
#include <wine/debug.h>

WINE_DEFAULT_DEBUG_CHANNEL(dwrite);

HRESULT WINAPI DWriteCreateFactory(DWORD factoryType, REFIID iid, void **factory)
{
    FIXME("(%u,%s,%p): stub\n", factoryType, wine_dbgstr_guid(iid), factory);
    if (factory) *factory = NULL;
    return E_NOTIMPL;
}
