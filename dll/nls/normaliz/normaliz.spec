@ stdcall IdnToAscii(long wstr long ptr long)
@ stdcall IdnToNameprepUnicode(long wstr long ptr long)
@ stdcall IdnToUnicode(long wstr long ptr long)
@ stdcall -version=0x500-0x502 IsNormalizedString(long wstr long)
@ stdcall -version=0x600+ IsNormalizedString(long wstr long) kernel32.IsNormalizedString
@ stdcall -version=0x500-0x502 NormalizeString(long wstr long ptr long)
@ stdcall -version=0x600+ NormalizeString(long wstr long ptr long) kernel32.NormalizeString
