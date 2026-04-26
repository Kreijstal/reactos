/*
 * PROJECT:     ReactOS API tests
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     Test for the private RDP input injection syscalls
 */

#include "../win32nt.h"

START_TEST(NtUserRdpInput)
{
    NTUSER_RDP_MOUSE_INPUT MouseInput;
    NTUSER_RDP_KEYBOARD_INPUT KeyboardInput;
    BOOL Ret;

    Ret = NtUserRdpInjectMouse(0, NULL);
    ok(!Ret, "NtUserRdpInjectMouse unexpectedly accepted NULL input\n");

    RtlZeroMemory(&MouseInput, sizeof(MouseInput));
    MouseInput.Size = sizeof(MouseInput);
    MouseInput.SessionId = 1;
    MouseInput.PointerFlags = NTUSER_RDP_MOUSE_FLAG_MOVE;
    Ret = NtUserRdpInjectMouse(1, &MouseInput);
    ok(!Ret, "NtUserRdpInjectMouse unexpectedly accepted session 1\n");

    MouseInput.SessionId = 0;
    MouseInput.PointerX = 4;
    MouseInput.PointerY = 5;
    Ret = NtUserRdpInjectMouse(0, &MouseInput);
    ok(Ret, "NtUserRdpInjectMouse failed for console move\n");

    Ret = NtUserRdpInjectKeyboard(0, NULL);
    ok(!Ret, "NtUserRdpInjectKeyboard unexpectedly accepted NULL input\n");

    RtlZeroMemory(&KeyboardInput, sizeof(KeyboardInput));
    KeyboardInput.Size = sizeof(KeyboardInput);
    KeyboardInput.SessionId = 1;
    KeyboardInput.KeyCode = 0x1e;
    Ret = NtUserRdpInjectKeyboard(1, &KeyboardInput);
    ok(!Ret, "NtUserRdpInjectKeyboard unexpectedly accepted session 1\n");

    KeyboardInput.SessionId = 0;
    Ret = NtUserRdpInjectKeyboard(0, &KeyboardInput);
    ok(Ret, "NtUserRdpInjectKeyboard failed for console key down\n");

    KeyboardInput.KeyboardFlags = NTUSER_RDP_KEYBOARD_FLAG_RELEASE;
    Ret = NtUserRdpInjectKeyboard(0, &KeyboardInput);
    ok(Ret, "NtUserRdpInjectKeyboard failed for console key up\n");
}
