extern "C" int __cdecl __cxa_guard_acquire(long long *Guard)
{
    return !(*(volatile char *)Guard);
}

extern "C" void __cdecl __cxa_guard_release(long long *Guard)
{
    *(volatile char *)Guard = 1;
}

extern "C" void __cdecl __cxa_guard_abort(long long *Guard)
{
    (void)Guard;
}
