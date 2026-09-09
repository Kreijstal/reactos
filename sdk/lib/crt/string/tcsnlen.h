
#include <stddef.h>
#include <tchar.h>

size_t __cdecl _tcsnlen(const _TCHAR * str, size_t count)
{
 const _TCHAR * s;

 if(str == 0) return 0;

 /* Check the count before dereferencing: reading one character past the
    limit faults on an unterminated buffer that ends at a page boundary */
 for(s = str; count && *s; ++ s, -- count);

 return s - str;
}

/* EOF */
