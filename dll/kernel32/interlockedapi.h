#pragma once

#include "types.h"

namespace kernel32 {

#ifdef WIBO_GUEST_64
struct alignas(16) SLIST_ENTRY {
#else
struct SLIST_ENTRY {
#endif
	GUEST_PTR Next;
};

using PSLIST_ENTRY = SLIST_ENTRY *;

#ifdef WIBO_GUEST_64
struct alignas(16) SLIST_HEADER {
	ULONGLONG Alignment;
	ULONGLONG Region;
};
static_assert(sizeof(SLIST_HEADER) == 16 && alignof(SLIST_HEADER) == 16);
static_assert(sizeof(SLIST_ENTRY) == 16);
static_assert(offsetof(SLIST_HEADER, Region) == 8);
#else
struct alignas(8) SLIST_HEADER {
	ULONGLONG Alignment;
};
static_assert(sizeof(SLIST_HEADER) == 8 && alignof(SLIST_HEADER) == 8);
static_assert(sizeof(SLIST_ENTRY) == 4 && alignof(SLIST_ENTRY) == 4);
#endif
static_assert(offsetof(SLIST_ENTRY, Next) == 0);

using PSLIST_HEADER = SLIST_HEADER *;

LONG WINAPI InterlockedIncrement(LONG volatile *Addend);
LONG WINAPI InterlockedDecrement(LONG volatile *Addend);
LONG WINAPI InterlockedExchange(LONG volatile *Target, LONG Value);
LONG WINAPI InterlockedCompareExchange(LONG volatile *Destination, LONG Exchange, LONG Comperand);
void WINAPI InitializeSListHead(PSLIST_HEADER ListHead);
// Node storage must remain readable during concurrent operations. Read retry
// after reclamation and legacy x64 Header8 encoding remain unsupported.
PSLIST_ENTRY WINAPI InterlockedPopEntrySList(PSLIST_HEADER ListHead);
PSLIST_ENTRY WINAPI InterlockedFlushSList(PSLIST_HEADER ListHead);

} // namespace kernel32
