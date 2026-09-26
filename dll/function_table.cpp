#include "ntdll.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "modules.h"

#ifdef WIBO_GUEST_64
#include <cstdlib>
#include <mutex>
#include <shared_mutex>

namespace {
struct DynamicFunctionTable {
	RUNTIME_FUNCTION *entries;
	DWORD count;
	DWORD capacity;
	ULONG_PTR base;
	ULONG_PTR end;
	DynamicFunctionTable *next;
};

std::shared_mutex g_functionTableMutex;
DynamicFunctionTable *g_functionTables = nullptr;

RUNTIME_FUNCTION *findEntry(RUNTIME_FUNCTION *entries, size_t count, ULONGLONG offset) {
	// Entries describe half-open ranges and are ordered by their beginning RVA.
	size_t first = 0;
	while (first < count) {
		const size_t middle = first + (count - first) / 2;
		if (offset < entries[middle].BeginAddress) {
			count = middle;
		} else if (offset >= entries[middle].EndAddress) {
			first = middle + 1;
		} else {
			return &entries[middle];
		}
	}
	return nullptr;
}
} // namespace

namespace ntdll {
NTSTATUS WINAPI RtlAddGrowableFunctionTable(GUEST_PTR *dynamicTable, RUNTIME_FUNCTION *functionTable, DWORD entryCount,
											DWORD maximumEntryCount, ULONG_PTR rangeBase, ULONG_PTR rangeEnd) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlAddGrowableFunctionTable(%p, %p, %u, %u, %llx, %llx)\n", dynamicTable, functionTable, entryCount,
			  maximumEntryCount, static_cast<unsigned long long>(rangeBase), static_cast<unsigned long long>(rangeEnd));
	if (!dynamicTable || !functionTable || entryCount > maximumEntryCount || rangeBase >= rangeEnd)
		return STATUS_INVALID_PARAMETER;
	auto *table = static_cast<DynamicFunctionTable *>(std::malloc(sizeof(DynamicFunctionTable)));
	if (!table)
		return static_cast<NTSTATUS>(0xC000009A); // STATUS_INSUFFICIENT_RESOURCES
	*table = {functionTable, entryCount, maximumEntryCount, rangeBase, rangeEnd, nullptr};
	std::unique_lock lock(g_functionTableMutex);
	auto **tail = &g_functionTables;
	while (*tail)
		tail = &(*tail)->next;
	*tail = table;
	*dynamicTable = toGuestPtr(table);
	return STATUS_SUCCESS;
}

VOID WINAPI RtlGrowFunctionTable(PVOID dynamicTable, DWORD newEntryCount) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlGrowFunctionTable(%p, %u)\n", dynamicTable, newEntryCount);
	std::unique_lock lock(g_functionTableMutex);
	for (auto *table = g_functionTables; table; table = table->next) {
		if (table == dynamicTable) {
			if (newEntryCount > table->count && newEntryCount <= table->capacity)
				table->count = newEntryCount;
			else
				DEBUG_LOG("RtlGrowFunctionTable: entry count must increase within capacity\n");
			return;
		}
	}
	DEBUG_LOG("RtlGrowFunctionTable: unknown registration\n");
}

VOID WINAPI RtlDeleteGrowableFunctionTable(PVOID dynamicTable) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlDeleteGrowableFunctionTable(%p)\n", dynamicTable);
	std::unique_lock lock(g_functionTableMutex);
	for (auto **link = &g_functionTables; *link; link = &(*link)->next) {
		if (*link == dynamicTable) {
			auto *table = *link;
			*link = table->next;
			std::free(table);
			return;
		}
	}
	DEBUG_LOG("RtlDeleteGrowableFunctionTable: unknown registration\n");
}

RUNTIME_FUNCTION *WINAPI RtlLookupFunctionEntry(ULONGLONG controlPc, ULONGLONG *imageBase, PVOID historyTable) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlLookupFunctionEntry(%llx, %p, %p)\n", controlPc, imageBase, historyTable);
	// A history table is an optional acceleration cache; always search current registrations.
	(void)historyTable;
	if (!imageBase)
		return nullptr;
	auto *module = wibo::moduleInfoFromAddress(reinterpret_cast<void *>(controlPc));
	if (module && module->executable) {
		const auto &image = *module->executable;
		const auto base = reinterpret_cast<uintptr_t>(image.imageBase);
		if (image.exceptionDirectoryRVA && image.exceptionDirectoryRVA <= image.imageSize &&
			image.exceptionDirectorySize <= image.imageSize - image.exceptionDirectoryRVA) {
			auto *entry = findEntry(image.fromRVA<RUNTIME_FUNCTION>(image.exceptionDirectoryRVA),
									image.exceptionDirectorySize / sizeof(RUNTIME_FUNCTION), controlPc - base);
			if (entry) {
				*imageBase = base;
				return entry;
			}
		}
	}
	std::shared_lock lock(g_functionTableMutex);
	for (auto *table = g_functionTables; table; table = table->next) {
		if (controlPc >= table->base && controlPc < table->end) {
			if (auto *entry = findEntry(table->entries, table->count, controlPc - table->base)) {
				*imageBase = table->base;
				return entry;
			}
		}
	}
	return nullptr;
}
} // namespace ntdll
#endif
