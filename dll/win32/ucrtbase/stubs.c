
#include <stdint.h>
#include <intrin.h>
#include <malloc.h>
#include <setjmp.h>
#include <string.h>
#include <wchar.h>
#define _USE_MATH_DEFINES
#include <math.h>

// atexit is needed by libsupc++
extern int __cdecl _crt_atexit(void (__cdecl*)(void));
int __cdecl atexit(void (__cdecl* function)(void))
{
    return _crt_atexit(function);
}

void* __cdecl operator_new(size_t size)
{
    return malloc(size);
}

void _cdecl operator_delete(void *mem)
{
    free(mem);
}

#ifdef _M_IX86
void _chkesp_failed(void)
{
    __debugbreak();
}
#endif

int __cdecl __acrt_initialize_sse2(void)
{
    return 0;
}

int __cdecl __acrt_initialize_fma3(void)
{
    return 0;
}

int __cdecl __intrinsic_setjmp(jmp_buf buffer)
{
    return _setjmp(buffer, NULL);
}

int __cdecl __intrinsic_setjmpex(jmp_buf buffer, void *frame)
{
    return _setjmp(buffer, frame);
}

unsigned int __cdecl _clearfp(void)
{
    return 0;
}

unsigned int __cdecl _statusfp(void)
{
    return 0;
}

unsigned int __cdecl _control87(unsigned int new_value, unsigned int mask)
{
    (void)new_value;
    (void)mask;
    return 0;
}

unsigned int __cdecl _controlfp(unsigned int new_value, unsigned int mask)
{
    return _control87(new_value, mask);
}

void __cdecl _set_FMA3_enable(int flag)
{
    (void)flag;
}

unsigned long __cdecl ucrtbase_lrotl(unsigned long value, int shift) __asm__("_lrotl");
unsigned long __cdecl ucrtbase_lrotl(unsigned long value, int shift)
{
    shift &= 0x1f;
    return (value << shift) | (value >> ((32 - shift) & 0x1f));
}

unsigned long __cdecl ucrtbase_lrotr(unsigned long value, int shift) __asm__("_lrotr");
unsigned long __cdecl ucrtbase_lrotr(unsigned long value, int shift)
{
    shift &= 0x1f;
    return (value >> shift) | (value << ((32 - shift) & 0x1f));
}

unsigned int __cdecl ucrtbase_rotl(unsigned int value, int shift) __asm__("_rotl");
unsigned int __cdecl ucrtbase_rotl(unsigned int value, int shift)
{
    shift &= 0x1f;
    return (value << shift) | (value >> ((32 - shift) & 0x1f));
}

unsigned int __cdecl ucrtbase_rotr(unsigned int value, int shift) __asm__("_rotr");
unsigned int __cdecl ucrtbase_rotr(unsigned int value, int shift)
{
    shift &= 0x1f;
    return (value >> shift) | (value << ((32 - shift) & 0x1f));
}

uint64_t __cdecl ucrtbase_rotl64(uint64_t value, int shift) __asm__("_rotl64");
uint64_t __cdecl ucrtbase_rotl64(uint64_t value, int shift)
{
    shift &= 0x3f;
    return (value << shift) | (value >> ((64 - shift) & 0x3f));
}

uint64_t __cdecl ucrtbase_rotr64(uint64_t value, int shift) __asm__("_rotr64");
uint64_t __cdecl ucrtbase_rotr64(uint64_t value, int shift)
{
    shift &= 0x3f;
    return (value >> shift) | (value << ((64 - shift) & 0x3f));
}

size_t __cdecl strnlen(const char *string, size_t maximum_count)
{
    size_t length = 0;

    while (length < maximum_count && string[length] != '\0')
        ++length;

    return length;
}

size_t __cdecl wcsnlen(const wchar_t *string, size_t maximum_count)
{
    size_t length = 0;

    while (length < maximum_count && string[length] != L'\0')
        ++length;

    return length;
}

// The following stubs cannot be implemented as stubs by spec2def, because they are intrinsics

#ifdef _MSC_VER
#pragma warning(disable:4163) // not available as an intrinsic function
#pragma warning(disable:4164) // intrinsic function not declared
#pragma function(fma)
#pragma function(fmaf)
#pragma function(log2)
#pragma function(log2f)
#pragma function(lrint)
#pragma function(lrintf)
#endif

double fma(double x, double y, double z)
{
    // Simplistic implementation
    return (x * y) + z;
}

float fmaf(float x, float y, float z)
{
    // Simplistic implementation
    return (x * y) + z;
}

double log2(double x)
{
    // Simplistic implementation: log2(x) = log(x) / log(2)
    return log(x) * M_LOG2E;
}

float log2f(float x)
{
    return (float)log2((double)x);
}

long int lrint(double x)
{
    __debugbreak();
    return 0;
}

long int lrintf(float x)
{
    __debugbreak();
    return 0;
}
