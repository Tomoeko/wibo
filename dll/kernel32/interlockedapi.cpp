#include "interlockedapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "internal.h"

#include <cstdio>
#ifdef WIBO_GUEST_64
#include <cpuid.h>
#endif

namespace {
#ifdef WIBO_GUEST_64
bool hostSupportsSListCompareExchange() {
	static const bool supported = [] {
		unsigned int eax, ebx, ecx, edx;
		return __get_cpuid(1, &eax, &ebx, &ecx, &edx) && (ecx & bit_CMPXCHG16B) != 0;
	}();
	return supported;
}

// Use the same full-width instruction as inline guest list mutations. A host
// mutex or library lock would not synchronize with those independent callers.
bool compareExchangeSList(kernel32::SLIST_HEADER *list, kernel32::SLIST_HEADER &expected,
						  const kernel32::SLIST_HEADER &desired) {
	unsigned char exchanged;
	__asm__ volatile("lock; cmpxchg16b %1; sete %0"
					 : "=q"(exchanged), "+m"(*list), "+a"(expected.Alignment), "+d"(expected.Region)
					 : "b"(desired.Alignment), "c"(desired.Region)
					 : "cc", "memory");
	return exchanged != 0;
}

kernel32::SLIST_HEADER loadSList(kernel32::SLIST_HEADER *list) {
	kernel32::SLIST_HEADER expected{};
	compareExchangeSList(list, expected, expected);
	return expected;
}
#else
static_assert(__atomic_always_lock_free(sizeof(ULONGLONG), nullptr));
#endif
} // namespace

namespace kernel32 {

LONG WINAPI InterlockedIncrement(LONG volatile *Addend) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("InterlockedIncrement(%p)\n", Addend);
	auto *ptr = const_cast<LONG *>(Addend);
	return __atomic_add_fetch(ptr, 1, __ATOMIC_SEQ_CST);
}

LONG WINAPI InterlockedDecrement(LONG volatile *Addend) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("InterlockedDecrement(%p)\n", Addend);
	auto *ptr = const_cast<LONG *>(Addend);
	return __atomic_sub_fetch(ptr, 1, __ATOMIC_SEQ_CST);
}

LONG WINAPI InterlockedExchange(LONG volatile *Target, LONG Value) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("InterlockedExchange(%p, %ld)\n", Target, static_cast<long>(Value));
	auto *ptr = const_cast<LONG *>(Target);
	return __atomic_exchange_n(ptr, Value, __ATOMIC_SEQ_CST);
}

LONG WINAPI InterlockedCompareExchange(LONG volatile *Destination, LONG Exchange, LONG Comperand) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("InterlockedCompareExchange(%p, %ld, %ld)\n", Destination, static_cast<long>(Exchange),
				static_cast<long>(Comperand));

	auto *ptr = const_cast<LONG *>(Destination);
	LONG expected = Comperand;
	__atomic_compare_exchange_n(ptr, &expected, Exchange, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return expected;
}

void WINAPI InitializeSListHead(PSLIST_HEADER ListHead) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("InitializeSListHead(%p)\n", ListHead);
	if (!ListHead) {
		return;
	}
	ListHead->Alignment = 0;
#ifdef WIBO_GUEST_64
	// Header16 stores its pointer in the upper word and its depth/sequence in
	// the lower word. The low bit of the upper word selects that representation.
	ListHead->Region = 1;
#endif
}

USHORT WINAPI QueryDepthSList(PSLIST_HEADER ListHead) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("QueryDepthSList(%p)\n", ListHead);
#ifdef WIBO_GUEST_64
	return static_cast<USHORT>(loadSList(ListHead).Alignment);
#else
	return static_cast<USHORT>(__atomic_load_n(&ListHead->Alignment, __ATOMIC_ACQUIRE) >> 32);
#endif
}

PSLIST_ENTRY WINAPI InterlockedPushEntrySList(PSLIST_HEADER ListHead, PSLIST_ENTRY ListEntry) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("InterlockedPushEntrySList(%p, %p)\n", ListHead, ListEntry);
#ifdef WIBO_GUEST_64
	if (!hostSupportsSListCompareExchange()) {
		std::fputs("Unsupported 128-bit compare-and-swap capability\n", stderr);
		exitInternal(ERROR_NOT_SUPPORTED);
	}
	auto expected = loadSList(ListHead);
	for (;;) {
		if (!(expected.Region & 1)) {
			std::fputs("Unsupported sequenced-list header encoding\n", stderr);
			exitInternal(ERROR_NOT_SUPPORTED);
		}
		auto *first = fromGuestPtr<SLIST_ENTRY>(expected.Region & ~ULONGLONG{0xf});
		__atomic_store_n(&ListEntry->Next, toGuestPtr(first), __ATOMIC_RELEASE);
		SLIST_HEADER desired{};
		desired.Alignment =
			((expected.Alignment & ~ULONGLONG{0xffff}) + 0x10000) | static_cast<WORD>(expected.Alignment + 1);
		desired.Region = (expected.Region & 0xf) | toGuestPtr(ListEntry);
		if (compareExchangeSList(ListHead, expected, desired))
			return first;
	}
#else
	ULONGLONG expected = __atomic_load_n(&ListHead->Alignment, __ATOMIC_ACQUIRE);
	for (;;) {
		auto *first = fromGuestPtr<SLIST_ENTRY>(static_cast<GUEST_PTR>(expected));
		__atomic_store_n(&ListEntry->Next, toGuestPtr(first), __ATOMIC_RELEASE);
		const auto depth = static_cast<WORD>((expected >> 32) + 1);
		const auto sequence = static_cast<WORD>((expected >> 48) + 1);
		const ULONGLONG desired = static_cast<ULONGLONG>(toGuestPtr(ListEntry)) |
								  (static_cast<ULONGLONG>(depth) << 32) | (static_cast<ULONGLONG>(sequence) << 48);
		if (__atomic_compare_exchange_n(&ListHead->Alignment, &expected, desired, false, __ATOMIC_SEQ_CST,
										__ATOMIC_ACQUIRE))
			return first;
	}
#endif
}

PSLIST_ENTRY WINAPI InterlockedPopEntrySList(PSLIST_HEADER ListHead) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("InterlockedPopEntrySList(%p)\n", ListHead);
#ifdef WIBO_GUEST_64
	if (!hostSupportsSListCompareExchange()) {
		std::fputs("Unsupported 128-bit compare-and-swap capability\n", stderr);
		exitInternal(ERROR_NOT_SUPPORTED);
	}
	auto expected = loadSList(ListHead);
	for (;;) {
		if (!(expected.Region & 1)) {
			// Legacy Header8 uses a different pointer encoding. Do not interpret
			// it as a modern header or report a nonempty unsupported list empty.
			std::fputs("Unsupported sequenced-list header encoding\n", stderr);
			exitInternal(ERROR_NOT_SUPPORTED);
		}
		auto *entry = fromGuestPtr<SLIST_ENTRY>(expected.Region & ~ULONGLONG{0xf});
		if (!entry) {
			return nullptr;
		}
		// The supported concurrency scope retains nodes until operations finish.
		// Retry after concurrent node reclamation requires guarded read support.
		const GUEST_PTR next = __atomic_load_n(&entry->Next, __ATOMIC_ACQUIRE);
		SLIST_HEADER desired{};
		desired.Alignment =
			((expected.Alignment & ~ULONGLONG{0xffff}) + 0x10000) | static_cast<WORD>(expected.Alignment - 1);
		desired.Region = (expected.Region & 0xf) | (next & ~GUEST_PTR{0xf});
		if (compareExchangeSList(ListHead, expected, desired)) {
			return entry;
		}
	}
#else
	ULONGLONG expected = __atomic_load_n(&ListHead->Alignment, __ATOMIC_ACQUIRE);
	for (;;) {
		auto *entry = fromGuestPtr<SLIST_ENTRY>(static_cast<GUEST_PTR>(expected));
		if (!entry) {
			return nullptr;
		}
		const GUEST_PTR next = __atomic_load_n(&entry->Next, __ATOMIC_ACQUIRE);
		const auto depth = static_cast<WORD>((expected >> 32) - 1);
		const auto sequence = static_cast<WORD>((expected >> 48) + 1);
		const ULONGLONG desired = static_cast<ULONGLONG>(next) | (static_cast<ULONGLONG>(depth) << 32) |
								  static_cast<ULONGLONG>(sequence) << 48;
		if (__atomic_compare_exchange_n(&ListHead->Alignment, &expected, desired, false, __ATOMIC_SEQ_CST,
										__ATOMIC_ACQUIRE)) {
			return entry;
		}
	}
#endif
}

PSLIST_ENTRY WINAPI InterlockedFlushSList(PSLIST_HEADER ListHead) {
	HOST_CONTEXT_GUARD();
	VERBOSE_LOG("InterlockedFlushSList(%p)\n", ListHead);
#ifdef WIBO_GUEST_64
	if (!hostSupportsSListCompareExchange()) {
		std::fputs("Unsupported 128-bit compare-and-swap capability\n", stderr);
		exitInternal(ERROR_NOT_SUPPORTED);
	}
	auto expected = loadSList(ListHead);
	for (;;) {
		if (!(expected.Region & 1)) {
			std::fputs("Unsupported sequenced-list header encoding\n", stderr);
			exitInternal(ERROR_NOT_SUPPORTED);
		}
		auto *first = fromGuestPtr<SLIST_ENTRY>(expected.Region & ~ULONGLONG{0xf});
		if (!first)
			return nullptr;
		SLIST_HEADER desired{};
		desired.Alignment = (expected.Alignment & ~ULONGLONG{0xffff}) + 0x10000;
		desired.Region = expected.Region & 0xf;
		if (compareExchangeSList(ListHead, expected, desired))
			return first;
	}
#else
	ULONGLONG expected = __atomic_load_n(&ListHead->Alignment, __ATOMIC_ACQUIRE);
	for (;;) {
		auto *first = fromGuestPtr<SLIST_ENTRY>(static_cast<GUEST_PTR>(expected));
		if (!first)
			return nullptr;
		const auto sequence = static_cast<WORD>((expected >> 48) + 1);
		const ULONGLONG desired = static_cast<ULONGLONG>(sequence) << 48;
		if (__atomic_compare_exchange_n(&ListHead->Alignment, &expected, desired, false, __ATOMIC_SEQ_CST,
										__ATOMIC_ACQUIRE))
			return first;
	}
#endif
}

} // namespace kernel32
