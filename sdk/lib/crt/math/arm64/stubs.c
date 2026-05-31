#include <precomp.h>

#define ARM64_MATH_STUB __attribute__((weak))

ARM64_MATH_STUB double
__cdecl
atan(double x)
{
    (void)x;
    return 0.0;
}

ARM64_MATH_STUB double
__cdecl
log(double x)
{
    (void)x;
    return 0.0;
}

ARM64_MATH_STUB double
__cdecl
log10(double x)
{
    (void)x;
    return 0.0;
}

ARM64_MATH_STUB double
__cdecl
pow(double x, double y)
{
    if (y == 0.0)
        return 1.0;

    if (y == 1.0)
        return x;

    return 0.0;
}

ARM64_MATH_STUB double
__cdecl
tan(double x)
{
    (void)x;
    return 0.0;
}

ARM64_MATH_STUB float
__cdecl
_hypotf(float x, float y)
{
    return x + y;
}

ARM64_MATH_STUB double
__cdecl
_logb(double x)
{
    (void)x;
    return 0.0;
}

ARM64_MATH_STUB float
__cdecl
_logbf(float x)
{
    (void)x;
    return 0.0f;
}

ARM64_MATH_STUB float
__cdecl
acosf(float x)
{
    (void)x;
    return 0.0f;
}

ARM64_MATH_STUB float
__cdecl
asinf(float x)
{
    (void)x;
    return 0.0f;
}

ARM64_MATH_STUB double
__cdecl
atan2(double y, double x)
{
    (void)y;
    (void)x;
    return 0.0;
}

ARM64_MATH_STUB float
__cdecl
atan2f(float y, float x)
{
    (void)y;
    (void)x;
    return 0.0f;
}

ARM64_MATH_STUB float
__cdecl
atanf(float x)
{
    (void)x;
    return 0.0f;
}

ARM64_MATH_STUB float
__cdecl
ceilf(float x)
{
    return (float)(long long)x;
}

ARM64_MATH_STUB float
__cdecl
coshf(float x)
{
    (void)x;
    return 1.0f;
}

ARM64_MATH_STUB double
__cdecl
exp(double x)
{
    (void)x;
    return 1.0;
}

ARM64_MATH_STUB float
__cdecl
expf(float x)
{
    (void)x;
    return 1.0f;
}

ARM64_MATH_STUB float
__cdecl
fabsf(float x)
{
    return x < 0.0f ? -x : x;
}

ARM64_MATH_STUB float
__cdecl
floorf(float x)
{
    return (float)(long long)x;
}

ARM64_MATH_STUB double
__cdecl
fmod(double x, double y)
{
    (void)y;
    return x;
}

ARM64_MATH_STUB float
__cdecl
fmodf(float x, float y)
{
    (void)y;
    return x;
}

ARM64_MATH_STUB double
__cdecl
ldexp(double x, int exp)
{
    while (exp > 0)
    {
        x *= 2.0;
        --exp;
    }

    while (exp < 0)
    {
        x *= 0.5;
        ++exp;
    }

    return x;
}

ARM64_MATH_STUB float
__cdecl
modff(float x, float *iptr)
{
    *iptr = (float)(long long)x;
    return x - *iptr;
}

ARM64_MATH_STUB float
__cdecl
sinf(float x)
{
    (void)x;
    return 0.0f;
}

ARM64_MATH_STUB float
__cdecl
sinhf(float x)
{
    (void)x;
    return 0.0f;
}

ARM64_MATH_STUB float
__cdecl
sqrtf(float x)
{
    return x;
}

ARM64_MATH_STUB float
__cdecl
tanf(float x)
{
    (void)x;
    return 0.0f;
}

ARM64_MATH_STUB float
__cdecl
tanhf(float x)
{
    (void)x;
    return 0.0f;
}
