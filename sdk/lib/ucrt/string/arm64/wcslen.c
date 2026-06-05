/*
 * ARM64 wcslen/wcsnlen implementation for toolchains that cannot assemble the
 * ARMASM UCRT string sources.
 */

#include <stddef.h>
#include <wchar.h>

size_t __cdecl
wcslen(const wchar_t *string)
{
    const wchar_t *current = string;

    while (*current != L'\0')
        ++current;

    return (size_t)(current - string);
}

size_t __cdecl
wcsnlen(const wchar_t *string, size_t maximum_count)
{
    size_t index = 0;

    while (index < maximum_count && string[index] != L'\0')
        ++index;

    return index;
}
