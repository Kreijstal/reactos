@ stdcall KdD0Transition()
@ stdcall KdD3Transition()
@ stdcall KdDebuggerInitialize0(ptr)
@ stdcall KdDebuggerInitialize1(ptr)
@ stdcall KdReceivePacket(long ptr ptr ptr ptr)
@ stdcall KdRestore(long)
@ stdcall KdSave(long)
@ stdcall KdSendPacket(long ptr ptr ptr)
# Adapter sharing (see sdk/include/reactos/kdnetshare.h).  Part of the transport
# contract because the boot loader loads every transport under this one name;
# a serial port has no adapter to share, so kdcom's implementations decline.
@ stdcall KdNetShareRegister(ptr)
@ stdcall KdNetShareDeregister()
@ stdcall KdNetShareQuery(ptr)
@ stdcall KdNetShareTransmit(ptr long)
@ stdcall KdNetSharePoll(long)
