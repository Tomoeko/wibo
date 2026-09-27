#include "test_assert.h"

#include <cpuid.h>
#include <stdint.h>
#include <windows.h>

typedef void(WINAPI *InitializeFn)(PSLIST_HEADER);
typedef PSLIST_ENTRY(WINAPI *EntryFn)(PSLIST_HEADER);

struct HeaderWords {
	ULONG_PTR low;
	ULONG_PTR high;
};

struct DECLSPEC_ALIGN(16) Node {
	SLIST_ENTRY entry;
	unsigned value;
	volatile LONG seen;
};

_Static_assert(sizeof(SLIST_HEADER) == 2 * sizeof(ULONG_PTR), "SLIST header width");
_Static_assert(sizeof(struct HeaderWords) == sizeof(SLIST_HEADER), "Atomic header snapshot width");

enum { PRODUCER_COUNT = 2, NODES_PER_PRODUCER = 128, NODE_COUNT = PRODUCER_COUNT * NODES_PER_PRODUCER };

static SLIST_HEADER DECLSPEC_ALIGN(16) head;
static struct Node nodes[NODE_COUNT];
static InitializeFn initialize;
static EntryFn flush, pop;
static HANDLE start, continue_producing, first_flushed, first_popped;
static volatile LONG producers_finished, consumed, flushed_nodes, popped_nodes;
#ifdef _WIN64
static const ULONGLONG sequence_mask = 0xffffffffffffULL;
#else
static const ULONGLONG sequence_mask = 0xffff;
#endif

static void check_instruction_support(void) {
	unsigned eax, ebx, ecx, edx;
	TEST_CHECK(__get_cpuid(1, &eax, &ebx, &ecx, &edx));
#ifdef _WIN64
	TEST_CHECK_MSG(ecx & bit_CMPXCHG16B, "Required 16-byte compare/exchange instruction is unavailable");
#else
	TEST_CHECK_MSG(edx & bit_CMPXCHG8B, "Required 8-byte compare/exchange instruction is unavailable");
#endif
}

static int compare_header(struct HeaderWords *expected, struct HeaderWords desired) {
	int success;
#ifdef _WIN64
	__asm__ volatile("lock cmpxchg16b %1"
#else
	__asm__ volatile("lock cmpxchg8b %1"
#endif
					 : "=@ccz"(success), "+m"(head), "+a"(expected->low), "+d"(expected->high)
					 : "b"(desired.low), "c"(desired.high)
					 : "memory");
	return success;
}

static struct HeaderWords read_header(void) {
	struct HeaderWords snapshot = {0, 0};
	const struct HeaderWords zero = {0, 0};
	(void)compare_header(&snapshot, zero);
	return snapshot;
}

static unsigned depth(struct HeaderWords snapshot) {
#ifdef _WIN64
	return (unsigned)(snapshot.low & 0xffff);
#else
	return (unsigned)(snapshot.high & 0xffff);
#endif
}

static ULONGLONG sequence(struct HeaderWords snapshot) {
#ifdef _WIN64
	return snapshot.low >> 16;
#else
	return snapshot.high >> 16;
#endif
}

static PSLIST_ENTRY first_entry(struct HeaderWords snapshot) {
#ifdef _WIN64
	return (PSLIST_ENTRY)(snapshot.high & ~(ULONG_PTR)15);
#else
	return (PSLIST_ENTRY)snapshot.low;
#endif
}

static struct HeaderWords make_header(PSLIST_ENTRY first, unsigned count, ULONGLONG counter) {
#ifdef _WIN64
	const struct HeaderWords result = {(ULONG_PTR)(counter << 16) | (count & 0xffff), (ULONG_PTR)first | 1};
#else
	const struct HeaderWords result = {(ULONG_PTR)first, ((ULONG_PTR)counter << 16) | (count & 0xffff)};
#endif
	return result;
}

static void initialize_head(void) {
	memset(&head, 0xa5, sizeof(head));
	SetLastError(0x72);
	initialize(&head);
	TEST_CHECK_EQ(0x72, GetLastError());
	const struct HeaderWords actual = read_header();
	const struct HeaderWords expected = make_header(NULL, 0, 0);
	TEST_CHECK_U64_EQ(expected.low, actual.low);
	TEST_CHECK_U64_EQ(expected.high, actual.high);
}

static void publish(PSLIST_ENTRY entry) {
	struct HeaderWords expected = read_header();
	for (;;) {
		entry->Next = first_entry(expected);
		struct HeaderWords desired = make_header(entry, depth(expected) + 1, sequence(expected) + 1);
#ifdef _WIN64
		desired.high = (desired.high & ~(ULONG_PTR)15) | (expected.high & 15);
#endif
		if (compare_header(&expected, desired))
			return;
	}
}

static PSLIST_ENTRY flush_checked(void) {
	SetLastError(0x72);
	PSLIST_ENTRY entry = flush(&head);
	TEST_CHECK_EQ(0x72, GetLastError());
	return entry;
}

static void check_empty(void) {
	const struct HeaderWords before = read_header();
	TEST_CHECK(flush_checked() == NULL);
	const struct HeaderWords after = read_header();
	TEST_CHECK_U64_EQ(before.low, after.low);
	TEST_CHECK_U64_EQ(before.high, after.high);
	TEST_CHECK_EQ(0, depth(after));
}

static void check_chain(unsigned count) {
	const struct HeaderWords before = read_header();
	TEST_CHECK_EQ(count, depth(before));
	PSLIST_ENTRY saved[3];
	for (unsigned index = 0; index < count; ++index)
		saved[index] = nodes[index].entry.Next;
	PSLIST_ENTRY first = flush_checked();
	TEST_CHECK(first == &nodes[count - 1].entry);
	PSLIST_ENTRY current = first;
	for (unsigned index = count; index-- > 0;) {
		TEST_CHECK(current == &nodes[index].entry);
		TEST_CHECK(current->Next == saved[index]);
		current = current->Next;
	}
	TEST_CHECK(current == NULL);
	const struct HeaderWords after = read_header();
	TEST_CHECK(first_entry(after) == NULL);
	TEST_CHECK_EQ(0, depth(after));
	TEST_CHECK_U64_EQ((sequence(before) + 1) & sequence_mask, sequence(after));
	printf("flush %u-bit: count=%u sequence=%llu->%llu expected=%llu\n", (unsigned)(sizeof(void *) * 8), count,
		   (unsigned long long)sequence(before), (unsigned long long)sequence(after),
		   (unsigned long long)((sequence(before) + 1) & sequence_mask));
#ifdef _WIN64
	TEST_CHECK_U64_EQ(before.high & 15, after.high & 15);
	printf("flush header mode: %llu->%llu\n", (unsigned long long)(before.high & 15),
		   (unsigned long long)(after.high & 15));
#endif
	check_empty();
}

static void consume_node(PSLIST_ENTRY entry, BOOL from_flush) {
	const uintptr_t offset = (uintptr_t)entry - (uintptr_t)&nodes[0];
	TEST_CHECK(offset < sizeof(nodes) && offset % sizeof(nodes[0]) == 0);
	const unsigned index = (unsigned)(offset / sizeof(nodes[0]));
	TEST_CHECK_EQ(index, nodes[index].value);
	TEST_CHECK_EQ(0, InterlockedCompareExchange(&nodes[index].seen, 1, 0));
	InterlockedIncrement(&consumed);
	if (from_flush)
		InterlockedIncrement(&flushed_nodes);
	else
		InterlockedIncrement(&popped_nodes);
}

static DWORD WINAPI producer(PVOID argument) {
	const unsigned first = (unsigned)(uintptr_t)argument * NODES_PER_PRODUCER;
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(start, 5000));
	for (unsigned index = 0; index < NODES_PER_PRODUCER; ++index) {
		nodes[first + index].value = first + index;
		publish(&nodes[first + index].entry);
		if (index == 0)
			TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(continue_producing, 5000));
	}
	InterlockedIncrement(&producers_finished);
	return 0;
}

static DWORD WINAPI flush_consumer(PVOID argument) {
	(void)argument;
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(start, 5000));
	BOOL initial = TRUE;
	for (;;) {
		const LONG finished = InterlockedCompareExchange(&producers_finished, 0, 0);
		PSLIST_ENTRY entry = flush_checked();
		if (!entry) {
			if (finished == PRODUCER_COUNT)
				return 0;
			Sleep(0);
			continue;
		}
		unsigned length = 0;
		while (entry) {
			TEST_CHECK(++length <= NODE_COUNT);
			consume_node(entry, TRUE);
			entry = entry->Next;
		}
		if (initial) {
			initial = FALSE;
			TEST_CHECK(SetEvent(first_flushed));
			TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(first_popped, 5000));
		}
	}
}

static DWORD WINAPI pop_consumer(PVOID argument) {
	(void)argument;
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(first_flushed, 5000));
	for (;;) {
		const LONG finished = InterlockedCompareExchange(&producers_finished, 0, 0);
		SetLastError(0x72);
		PSLIST_ENTRY entry = pop(&head);
		TEST_CHECK_EQ(0x72, GetLastError());
		if (!entry) {
			if (finished == PRODUCER_COUNT)
				return 0;
			Sleep(0);
			continue;
		}
		consume_node(entry, FALSE);
		TEST_CHECK(SetEvent(first_popped));
	}
}

static void check_concurrent(void) {
	initialize_head();
	memset(nodes, 0, sizeof(nodes));
	start = CreateEventA(NULL, TRUE, FALSE, NULL);
	continue_producing = CreateEventA(NULL, TRUE, FALSE, NULL);
	first_flushed = CreateEventA(NULL, TRUE, FALSE, NULL);
	first_popped = CreateEventA(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(start && continue_producing && first_flushed && first_popped);
	HANDLE workers[PRODUCER_COUNT + 2];
	for (unsigned index = 0; index < PRODUCER_COUNT; ++index)
		workers[index] = CreateThread(NULL, 0, producer, (PVOID)(uintptr_t)index, 0, NULL);
	workers[PRODUCER_COUNT] = CreateThread(NULL, 0, flush_consumer, NULL, 0, NULL);
	workers[PRODUCER_COUNT + 1] = CreateThread(NULL, 0, pop_consumer, NULL, 0, NULL);
	for (unsigned index = 0; index < PRODUCER_COUNT + 2; ++index)
		TEST_CHECK(workers[index] != NULL);
	TEST_CHECK(SetEvent(start));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(first_flushed, 5000));
	TEST_CHECK(SetEvent(continue_producing));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForMultipleObjects(PRODUCER_COUNT + 2, workers, TRUE, 10000));
	for (unsigned index = 0; index < PRODUCER_COUNT + 2; ++index) {
		DWORD code;
		TEST_CHECK(GetExitCodeThread(workers[index], &code));
		TEST_CHECK_EQ(0, code);
		TEST_CHECK(CloseHandle(workers[index]));
	}
	TEST_CHECK_EQ(NODE_COUNT, consumed);
	TEST_CHECK(flushed_nodes > 0 && popped_nodes > 0);
	for (unsigned index = 0; index < NODE_COUNT; ++index)
		TEST_CHECK_EQ(1, nodes[index].seen);
	const struct HeaderWords final = read_header();
	TEST_CHECK(first_entry(final) == NULL);
	TEST_CHECK_EQ(0, depth(final));
	printf("flush concurrent: flushed=%ld popped=%ld total=%ld sequence=%llu\n", (long)flushed_nodes,
		   (long)popped_nodes, (long)consumed, (unsigned long long)sequence(final));
	check_empty();
	TEST_CHECK(CloseHandle(start));
	TEST_CHECK(CloseHandle(continue_producing));
	TEST_CHECK(CloseHandle(first_flushed));
	TEST_CHECK(CloseHandle(first_popped));
}

int main(void) {
	check_instruction_support();
	HMODULE module = GetModuleHandleW(L"kernel32.dll");
	TEST_CHECK(module != NULL);
	FARPROC exported = GetProcAddress(module, "InitializeSListHead");
	_Static_assert(sizeof(exported) == sizeof(initialize), "Initialization function pointer width");
	memcpy(&initialize, &exported, sizeof(initialize));
	exported = GetProcAddress(module, "InterlockedPopEntrySList");
	_Static_assert(sizeof(exported) == sizeof(pop), "Pop function pointer width");
	memcpy(&pop, &exported, sizeof(pop));
	exported = GetProcAddress(module, "InterlockedFlushSList");
	_Static_assert(sizeof(exported) == sizeof(flush), "Flush function pointer width");
	memcpy(&flush, &exported, sizeof(flush));
	TEST_CHECK(initialize && pop && flush);
	TEST_CHECK((uintptr_t)&head % MEMORY_ALLOCATION_ALIGNMENT == 0);
	TEST_CHECK((uintptr_t)&nodes[0].entry % MEMORY_ALLOCATION_ALIGNMENT == 0);
	initialize_head();
	check_empty();
	for (unsigned index = 0; index < 3; ++index)
		publish(&nodes[index].entry);
	check_chain(3);
	for (unsigned index = 0; index < 2; ++index)
		publish(&nodes[index].entry);
	const struct HeaderWords boundary = make_header(&nodes[1].entry, 2, sequence_mask);
	memcpy(&head, &boundary, sizeof(head));
	check_chain(2);
	check_concurrent();
	return 0;
}
