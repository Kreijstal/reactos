/*
 * PROJECT:     ReactOS AMD Radeon ATOM-BIOS Framebuffer Miniport
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Temporary bring-up stubs.  Replaced by radeon_bios.c,
 *              radeon_atombios.c and radeon_modeset.c in the ATOM
 *              modesetting stage; while these are in place HwFindAdapter
 *              declines every DCE adapter and only the display-less Hainan
 *              off-screen surface path is reachable.
 * COPYRIGHT:   Copyright 2026 Kreijstal <elektrischrainbow@gmail.com>
 */

#include "radeon.h"

BOOLEAN
RadeonGetBios(
    _Inout_ PRADEONFB_DEVICE_EXTENSION DeviceExtension)
{
    UNREFERENCED_PARAMETER(DeviceExtension);
    return FALSE;
}

BOOLEAN
RadeonAtomGetClockInfo(
    _Inout_ PRADEONFB_DEVICE_EXTENSION DeviceExtension)
{
    UNREFERENCED_PARAMETER(DeviceExtension);
    return FALSE;
}

BOOLEAN
RadeonAtomGetLvdsInfo(
    _Inout_ PRADEONFB_DEVICE_EXTENSION DeviceExtension)
{
    UNREFERENCED_PARAMETER(DeviceExtension);
    return FALSE;
}

BOOLEAN
RadeonAtomParseObjectTable(
    _Inout_ PRADEONFB_DEVICE_EXTENSION DeviceExtension)
{
    UNREFERENCED_PARAMETER(DeviceExtension);
    return FALSE;
}

BOOLEAN
RadeonSetMode(
    _Inout_ PRADEONFB_DEVICE_EXTENSION DeviceExtension,
    _In_ PRADEONFB_TIMING Timing)
{
    UNREFERENCED_PARAMETER(DeviceExtension);
    UNREFERENCED_PARAMETER(Timing);
    return FALSE;
}
