/*
 * swdevice.h - Software device installation
 *
 * This file is part of the ReactOS PSDK package.
 */

#pragma once

#include <devpropdef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if (NTDDI_VERSION >= NTDDI_WIN8)

DECLARE_HANDLE(HSWDEVICE);
typedef HSWDEVICE *PHSWDEVICE;

typedef enum _SW_DEVICE_CAPABILITIES
{
    SWDeviceCapabilitiesNone = 0x00000000,
    SWDeviceCapabilitiesRemovable = 0x00000001,
    SWDeviceCapabilitiesSilentInstall = 0x00000002,
    SWDeviceCapabilitiesNoDisplayInUI = 0x00000004,
    SWDeviceCapabilitiesDriverRequired = 0x00000008
} SW_DEVICE_CAPABILITIES, *PSW_DEVICE_CAPABILITIES;

typedef enum _SW_DEVICE_LIFETIME
{
    SWDeviceLifetimeHandle = 0,
    SWDeviceLifetimeParentPresent = 1,
    SWDeviceLifetimeMax = 2
} SW_DEVICE_LIFETIME, *PSW_DEVICE_LIFETIME;

typedef struct _SW_DEVICE_CREATE_INFO
{
    ULONG cbSize;
    PCWSTR pszInstanceId;
    PCZZWSTR pszzHardwareIds;
    PCZZWSTR pszzCompatibleIds;
    const GUID *pContainerId;
    ULONG CapabilityFlags;
    PCWSTR pszDeviceDescription;
    PCWSTR pszDeviceLocation;
    const SECURITY_DESCRIPTOR *pSecurityDescriptor;
} SW_DEVICE_CREATE_INFO, *PSW_DEVICE_CREATE_INFO;

typedef VOID(CALLBACK *SW_DEVICE_CREATE_CALLBACK)(
    _In_ HSWDEVICE hSwDevice,
    _In_ HRESULT CreateResult,
    _In_opt_ PVOID pContext,
    _In_opt_ PCWSTR pszDeviceInstanceId);

HRESULT
WINAPI
SwDeviceCreate(
    _In_ PCWSTR pszEnumeratorName,
    _In_ PCWSTR pszParentDeviceInstance,
    _In_ const SW_DEVICE_CREATE_INFO *pCreateInfo,
    _In_ ULONG cPropertyCount,
    _In_reads_opt_(cPropertyCount) const DEVPROPERTY *pProperties,
    _In_ SW_DEVICE_CREATE_CALLBACK pCallback,
    _In_opt_ PVOID pContext,
    _Outptr_ PHSWDEVICE phSwDevice);

VOID
WINAPI
SwDeviceClose(
    _In_ HSWDEVICE hSwDevice);

HRESULT
WINAPI
SwDeviceSetLifetime(
    _In_ HSWDEVICE hSwDevice,
    _In_ SW_DEVICE_LIFETIME Lifetime);

HRESULT
WINAPI
SwDeviceGetLifetime(
    _In_ HSWDEVICE hSwDevice,
    _Out_ PSW_DEVICE_LIFETIME pLifetime);

#endif /* (NTDDI_VERSION >= NTDDI_WIN8) */

#ifdef __cplusplus
}
#endif
