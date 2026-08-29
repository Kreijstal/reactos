@ stdcall KdD0Transition()
@ stdcall KdD3Transition()
@ stdcall KdDebuggerInitialize0(ptr)
@ stdcall KdDebuggerInitialize1(ptr)
@ stdcall KdReceivePacket(long ptr ptr ptr ptr)
@ stdcall KdRestore(long)
@ stdcall KdSave(long)
@ stdcall KdSendPacket(long ptr ptr ptr)
# Adapter sharing (see sdk/include/reactos/kdnetshare.h).  Consumed by
# kdnetshare.sys, the NDIS miniport that gives the OS a network on the NIC the
# debugger owns.  Inert unless /KDNETSHARE is in the boot options.
@ stdcall KdNetShareRegister(ptr)
@ stdcall KdNetShareDeregister()
@ stdcall KdNetShareQuery(ptr)
@ stdcall KdNetShareTransmit(ptr long)
@ stdcall KdNetSharePoll(long)
@ stdcall KdNetShareRingStats(ptr)
