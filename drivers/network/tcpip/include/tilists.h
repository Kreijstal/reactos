#pragma once

#define TIPASTE(x,y) x ## y

#define IF_LIST_ITER(n) \
    PLIST_ENTRY TIPASTE(n,Entry); \
    PIP_INTERFACE n;

#define ForEachInterface(n) \
    TIPASTE(n,Entry) = InterfaceListHead.Flink; \
    while (TIPASTE(n,Entry) != &InterfaceListHead) { \
              ASSERT(TIPASTE(n,Entry)); \
	      n = CONTAINING_RECORD(TIPASTE(n,Entry), IP_INTERFACE, \
				    ListEntry); \
	      ASSERT(n);

#define EndFor(n) \
     TIPASTE(n,Entry) = TIPASTE(n,Entry)->Flink; \
}
