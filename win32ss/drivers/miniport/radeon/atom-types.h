/*
 * Copyright 2008 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Author: Dave Airlie
 *
 * Imported from Linux v6.6 drivers/gpu/drm/radeon/atom-types.h for the
 * ReactOS radeonfb miniport.  ReactOS port change: the USHORT/ULONG/UCHAR
 * typedefs are omitted because the NT DDK headers already provide them with
 * the sizes AtomBIOS expects (32-bit ULONG on both i386 and amd64); only the
 * endianness selection remains, pinned to little-endian.
 */

#ifndef ATOM_TYPES_H
#define ATOM_TYPES_H

/* sync atom types to kernel types */

#ifndef ATOM_BIG_ENDIAN
#define ATOM_BIG_ENDIAN 0
#endif

#endif
