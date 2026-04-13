/*
 * 32-bit WoW64 test program with CRT support.
 * Uses printf (via ReactOS 32-bit msvcrt.dll in SysWOW64).
 */
#include <windows.h>
#include <stdio.h>

int main(void)
{
    printf("Hello from 32-bit WoW64!\n");
    OutputDebugStringA("=== hello32: printf test completed ===\n");
    return 0;
}
