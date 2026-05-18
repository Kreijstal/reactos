@ stdcall QueryInterruptTime(ptr)
@ stdcall QueryInterruptTimePrecise(ptr)
@ stdcall QueryUnbiasedInterruptTime(ptr)
@ stdcall QueryUnbiasedInterruptTimePrecise(ptr)
@ stdcall SetThreadDescription(ptr wstr)
@ stdcall VirtualAlloc2(ptr ptr long long long ptr long)
@ stdcall -version=0x602+ WaitOnAddress(ptr ptr long long)
@ stdcall -version=0x602+ WakeByAddressAll(ptr) ntdll.RtlWakeAddressAll
@ stdcall -version=0x602+ WakeByAddressSingle(ptr) ntdll.RtlWakeAddressSingle
