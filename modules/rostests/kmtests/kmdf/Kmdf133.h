/*
 * PROJECT:     ReactOS kernel-mode tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     KMDF 1.33 client test declarations
 * COPYRIGHT:   Copyright 2026 ReactOS contributors
 */

#pragma once

#define IOCTL_KMDF133_RUN_TESTS 1

/* Service of the test class extension: "<class name><class major><KMDF major>" */
#define KMTCX_SERVICE_NAME L"KmtCx0101"
#define KMTCX_IMAGE_NAME   L"kmdfcx_drv.sys"

/* A class extension with a service whose image is missing */
#define KMTNOCX_CLASS_NAME   L"KmtNoCx"
#define KMTNOCX_SERVICE_NAME L"KmtNoCx0101"
#define KMTNOCX_IMAGE_PATH   L"\\SystemRoot\\system32\\drivers\\kmdfnocx_drv.sys"
