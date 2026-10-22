/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     UCX class extension binding test declarations
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 */

#pragma once

#define IOCTL_KMDFUCX_RUN_TESTS 1

/* WDFLDR's default service name for class "Ucx" 1.x on KMDF 1.x */
#define UCX_SERVICE_NAME L"Ucx0101"
#define UCX_IMAGE_NAME   L"ucx01000.sys"
