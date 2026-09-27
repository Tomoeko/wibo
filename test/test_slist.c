#include "test_assert.h"

#include <cpuid.h>
#include <stdint.h>
#include <windows.h>

typedef void(WINAPI *initialize_fn)(PSLIST_HEADER);
typedef PSLIST_ENTRY(WINAPI *pop_fn)(PSLIST_HEADER);

struct header_words {
	ULONG_PTR low;
	ULONG_PTR high;
};

struct DECLSPEC_ALIGN(16) node {
	SLIST_ENTRY entry;
	unsigned value;
	volatile LONG seen;
};

_Static_assert(sizeof(SLIST_HEADER) == 2 * sizeof(ULONG_PTR), "SLIST header width");
_Static_assert(sizeof(struct header_words) == sizeof(SLIST_HEADER), "SLIST snapshot width");

enum { PRODUCER_COUNT = 2, CONSUMER_COUNT = 2, NODES_PER_PRODUCER = 256, NODE_COUNT = 512 };
static SLIST_HEADER DECLSPEC_ALIGN(16) head;
static struct node nodes[NODE_COUNT];
static initialize_fn initialize;
static pop_fn pop;
static HANDLE start, continue_producing, first_consumed;
static volatile LONG producers_finished, consumed;
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

static int compare_header(struct header_words *expected, struct header_words desired) {
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

static struct header_words read_header(void) {
	struct header_words snapshot = {0, 0};
	const struct header_words zero = {0, 0};
	(void)compare_header(&snapshot, zero);
	return snapshot;
}

static unsigned depth(struct header_words snapshot) {
#ifdef _WIN64
	return (unsigned)(snapshot.low & 0xffff);
#else
	return (unsigned)(snapshot.high & 0xffff);
#endif
}

static ULONGLONG sequence(struct header_words snapshot) {
#ifdef _WIN64
	return snapshot.low >> 16;
#else
	return snapshot.high >> 16;
#endif
}

static PSLIST_ENTRY first_entry(struct header_words snapshot) {
#ifdef _WIN64
	return (PSLIST_ENTRY)(snapshot.high & ~(ULONG_PTR)15);
#else
	return (PSLIST_ENTRY)snapshot.low;
#endif
}

static struct header_words make_header(PSLIST_ENTRY first, unsigned count, ULONGLONG counter) {
#ifdef _WIN64
	const struct header_words result = {(ULONG_PTR)(counter << 16) | (count & 0xffff), (ULONG_PTR)first | 1};
#else
	const struct header_words result = {(ULONG_PTR)first, ((ULONG_PTR)counter << 16) | (count & 0xffff)};
#endif
	return result;
}

static void initialize_head(void) {
	memset(&head, 0xa5, sizeof(head));
	SetLastError(0x72);
	initialize(&head);
	TEST_CHECK_EQ(0x72, GetLastError());
	const struct header_words actual = read_header();
	const struct header_words expected = make_header(NULL, 0, 0);
	TEST_CHECK_U64_EQ(expected.low, actual.low);
	TEST_CHECK_U64_EQ(expected.high, actual.high);
}

static void check_empty(void) {
	const struct header_words before = read_header();
	SetLastError(0x72);
	TEST_CHECK(pop(&head) == NULL);
	TEST_CHECK_EQ(0x72, GetLastError());
	const struct header_words after = read_header();
	TEST_CHECK_U64_EQ(before.low, after.low);
	TEST_CHECK_U64_EQ(before.high, after.high);
	TEST_CHECK_EQ(0, depth(after));
}

static void publish(PSLIST_ENTRY entry) {
	struct header_words expected = read_header();
	for (;;) {
		entry->Next = first_entry(expected);
		struct header_words desired = make_header(entry, depth(expected) + 1, sequence(expected) + 1);
#ifdef _WIN64
		desired.high = (desired.high & ~(ULONG_PTR)15) | (expected.high & 15);
#endif
		if (compare_header(&expected, desired))
			return;
	}
}

static void check_pop(PSLIST_ENTRY expected_entry) {
	const struct header_words before = read_header();
	PSLIST_ENTRY expected_next = expected_entry->Next;
	SetLastError(0x72);
	PSLIST_ENTRY actual = pop(&head);
	TEST_CHECK_EQ(0x72, GetLastError());
	TEST_CHECK(actual == expected_entry);
	TEST_CHECK(actual->Next == expected_next);
	const struct header_words after = read_header();
	TEST_CHECK(first_entry(after) == expected_next);
	TEST_CHECK_EQ(depth(before) - 1, depth(after));
	TEST_CHECK_U64_EQ((sequence(before) + 1) & sequence_mask, sequence(after));
	printf("SLIST %u-bit pop sequence: %llu -> %llu\n", (unsigned)(sizeof(void *) * 8),
		   (unsigned long long)sequence(before), (unsigned long long)sequence(after));
#ifdef _WIN64
	printf("SLIST header mode: %llu -> %llu\n", (unsigned long long)(before.high & 15),
		   (unsigned long long)(after.high & 15));
#endif
}

static DWORD WINAPI producer(PVOID argument) {
	const unsigned first = (unsigned)(uintptr_t)argument * NODES_PER_PRODUCER;
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(start, 5000));
	for (unsigned i = 0; i < NODES_PER_PRODUCER; ++i) {
		nodes[first + i].value = first + i;
		publish(&nodes[first + i].entry);
		if (i == 0)
			TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(continue_producing, 5000));
	}
	InterlockedIncrement(&producers_finished);
	return 0;
}

static DWORD WINAPI consumer(PVOID argument) {
	(void)argument;
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(start, 5000));
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
		const uintptr_t offset = (uintptr_t)entry - (uintptr_t)&nodes[0];
		TEST_CHECK(offset < sizeof(nodes) && offset % sizeof(nodes[0]) == 0);
		const unsigned index = (unsigned)(offset / sizeof(nodes[0]));
		TEST_CHECK_EQ(index, nodes[index].value);
		TEST_CHECK_EQ(0, InterlockedCompareExchange(&nodes[index].seen, 1, 0));
		if (InterlockedIncrement(&consumed) == 1)
			TEST_CHECK(SetEvent(first_consumed));
	}
}

static void check_concurrent(void) {
	initialize_head();
	memset(nodes, 0, sizeof(nodes));
	start = CreateEventA(NULL, TRUE, FALSE, NULL);
	continue_producing = CreateEventA(NULL, TRUE, FALSE, NULL);
	first_consumed = CreateEventA(NULL, TRUE, FALSE, NULL);
	TEST_CHECK(start && continue_producing && first_consumed);
	HANDLE workers[PRODUCER_COUNT + CONSUMER_COUNT];
	for (unsigned i = 0; i < PRODUCER_COUNT + CONSUMER_COUNT; ++i) {
		workers[i] = CreateThread(NULL, 0, i < PRODUCER_COUNT ? producer : consumer, (PVOID)(uintptr_t)i, 0, NULL);
		TEST_CHECK(workers[i] != NULL);
	}
	TEST_CHECK(SetEvent(start));
	// Producers retain their activation until a consumer has removed an entry.
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(first_consumed, 5000));
	TEST_CHECK(SetEvent(continue_producing));
	TEST_CHECK_EQ(WAIT_OBJECT_0, WaitForMultipleObjects(PRODUCER_COUNT + CONSUMER_COUNT, workers, TRUE, 10000));
	for (unsigned i = 0; i < PRODUCER_COUNT + CONSUMER_COUNT; ++i) {
		DWORD code;
		TEST_CHECK(GetExitCodeThread(workers[i], &code));
		TEST_CHECK_EQ(0, code);
		TEST_CHECK(CloseHandle(workers[i]));
	}
	TEST_CHECK_EQ(NODE_COUNT, consumed);
	TEST_CHECK_U64_EQ(2 * NODE_COUNT, sequence(read_header()));
	for (unsigned i = 0; i < NODE_COUNT; ++i)
		TEST_CHECK_EQ(1, nodes[i].seen);
	const struct header_words final = read_header();
	TEST_CHECK(first_entry(final) == NULL);
	printf("SLIST concurrent sequence: %llu\n", (unsigned long long)sequence(final));
	check_empty();
	TEST_CHECK(CloseHandle(start));
	TEST_CHECK(CloseHandle(continue_producing));
	TEST_CHECK(CloseHandle(first_consumed));
}

int main(void) {
	check_instruction_support();
	HMODULE module = GetModuleHandleA("kernel32.dll");
	TEST_CHECK(module != NULL);
	initialize = (initialize_fn)(uintptr_t)GetProcAddress(module, "InitializeSListHead");
	pop = (pop_fn)(uintptr_t)GetProcAddress(module, "InterlockedPopEntrySList");
	TEST_CHECK(initialize && pop);
	TEST_CHECK((uintptr_t)&head % MEMORY_ALLOCATION_ALIGNMENT == 0);
	TEST_CHECK((uintptr_t)&nodes[0].entry % MEMORY_ALLOCATION_ALIGNMENT == 0);
	initialize_head();
	check_empty();
	for (unsigned i = 0; i < 3; ++i)
		publish(&nodes[i].entry);
	for (unsigned i = 3; i-- > 0;)
		check_pop(&nodes[i].entry);
	check_empty();
	// Set a coherent one-entry header at the sequence boundary while quiescent.
	nodes[0].entry.Next = NULL;
	const struct header_words boundary = make_header(&nodes[0].entry, 1, sequence_mask);
	memcpy(&head, &boundary, sizeof(head));
	check_pop(&nodes[0].entry);
	check_empty();
	// Every concurrent node is published once and remains alive through all joins.
	check_concurrent();
	return EXIT_SUCCESS;
}
