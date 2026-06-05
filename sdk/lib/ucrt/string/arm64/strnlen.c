/*
 * ARM64 strnlen implementation for toolchains that cannot assemble the
 * ARMASM UCRT string sources.
 */

#include <string.h>

size_t __cdecl
strnlen(const char *string, size_t maximum_count)
{
    size_t index = 0;

    while (index < maximum_count && string[index] != '\0')
        ++index;

    return index;
}
