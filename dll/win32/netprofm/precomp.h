
#ifndef _NETPROFM_PRECOMP_H_
#define _NETPROFM_PRECOMP_H_

#include <wine/config.h>

#include <stdarg.h>

#define WIN32_NO_STATUS
#define _INC_WINDOWS
#define COM_NO_WINDOWS_H

#define COBJMACROS

#include <windef.h>
#include <winbase.h>
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <ifdef.h>
#include <netioapi.h>
#include <initguid.h>
#include <objbase.h>
#include <ocidl.h>
#include <olectl.h>
#include <rpcproxy.h>
#include <netlistmgr.h>

#include <wine/debug.h>
#include <wine/list.h>

#include "netprofm_private.h"

#endif /* !_NETPROFM_PRECOMP_H_ */
