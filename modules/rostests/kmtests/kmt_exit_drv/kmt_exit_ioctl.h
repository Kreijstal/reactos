/*
 * PROJECT:     ReactOS kernel-mode test runner
 * LICENSE:     LGPL-2.1+ (https://spdx.org/licenses/LGPL-2.1+)
 * PURPOSE:     QEMU debug-exit driver public interface
 *
 * The runner uses this driver to terminate QEMU at the end of the test pass
 * by writing a byte to IO port 0xF4 (QEMU's isa-debug-exit device). QEMU then
 * exits with code (value << 1) | 1.
 */

#ifndef _KMT_EXIT_IOCTL_H_
#define _KMT_EXIT_IOCTL_H_

#define KMT_EXIT_DEVICE_NAME        L"KmtExit"
#define KMT_EXIT_DEVICE_DRIVER_PATH L"\\Device\\" KMT_EXIT_DEVICE_NAME
#define KMT_EXIT_SERVICE_NAME       L"KmtExit"

/* Input: UCHAR status byte. Caller never returns from the IOCTL: QEMU exits. */
#define IOCTL_KMT_EXIT_QEMU \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_WRITE_DATA)

#endif /* _KMT_EXIT_IOCTL_H_ */
