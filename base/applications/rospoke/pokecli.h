/*
 * PROJECT:     ReactOS hardware bring-up poke driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     rospoke.exe internal declarations
 * COPYRIGHT:   Copyright 2026 the ReactOS contributors
 */

#ifndef _POKECLI_H_
#define _POKECLI_H_

#include <windows.h>
#include <winioctl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <reactos/rospoke.h>

#define POKE_EXIT_OK        0
#define POKE_EXIT_USAGE     1
#define POKE_EXIT_DRIVER    2
#define POKE_EXIT_SCRIPT    3
#define POKE_EXIT_HARDWARE  4

/* service.c */
int PokeServiceInstall(const char *ImagePath);
int PokeServiceRemove(void);
int PokeServiceStart(void);
int PokeServiceStop(void);
int PokeServiceStatus(void);

void PokePrintLastError(const char *What);

#endif /* _POKECLI_H_ */
