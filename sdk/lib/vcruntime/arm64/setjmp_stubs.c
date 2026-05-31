#include <setjmp.h>

int
__cdecl
_setjmp(jmp_buf Buffer, void *Frame)
{
    (void)Buffer;
    (void)Frame;
    return 0;
}

int
__cdecl
_setjmpex(jmp_buf Buffer, void *Frame)
{
    return _setjmp(Buffer, Frame);
}
