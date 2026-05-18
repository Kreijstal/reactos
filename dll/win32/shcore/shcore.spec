@ stdcall CommandLineToArgvW(wstr ptr) shcore_CommandLineToArgvW
@ stdcall GetDpiForMonitor(ptr long ptr ptr)
@ stdcall GetProcessDpiAwareness(ptr ptr)
@ stdcall SetProcessDpiAwareness(long)
@ stdcall GetScaleFactorForDevice(long)
@ stdcall GetScaleFactorForMonitor(ptr ptr)
@ stdcall SetCurrentProcessExplicitAppUserModelID(wstr)
@ stdcall GetCurrentProcessExplicitAppUserModelID(ptr)
@ stub RegisterScaleChangeEvent
@ stub UnregisterScaleChangeEvent
@ stub RegisterScaleChangeNotifications
@ stub RevokeScaleChangeNotifications
@ stub SHCreateMemStream
@ stub SHCreateStreamOnFileEx
@ stub SHCreateStreamOnFileW
@ stub IStream_Read
@ stub IStream_Write
@ stub IStream_Reset
@ stub IStream_Size
@ stub IsOS
