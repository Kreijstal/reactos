/*
 * PROJECT:     ReactOS crt library
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Implementation of sprintf_s
 */

#define _sxprintf sprintf_s
#define USE_COUNT 0
#define IS_SECAPI 1

#include "_sxprintf.c"
