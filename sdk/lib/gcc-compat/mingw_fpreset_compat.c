/* SPDX-License-Identifier: GPL-2.0-or-later */

/* MinGW fenv.h uses this two-word ABI. Its CRT_fp8.c encodes the default
 * x87 control word 0x027f and MXCSR 0x1f80 as below. These are also the
 * defaults set by ReactOS's i386 _fpreset. Supplying the data separately
 * avoids pulling MinGW's conflicting _fpreset implementation into the image.
 */
typedef struct
{
    unsigned long control;
    unsigned long status;
} mingw_fenv_t;

const mingw_fenv_t __mingw_fe_dfl_env = { 0x3f3f103fUL, 0 };
