#include "ntdll.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "modules.h"

#ifdef WIBO_GUEST_64
#include "ntdll_trampolines.h"

#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {
struct DynamicFunctionTable {
	RUNTIME_FUNCTION *entries;
	DWORD count;
	DWORD capacity;
	ULONG_PTR base;
	ULONG_PTR end;
	ULONGLONG identifier;
	PGET_RUNTIME_FUNCTION_CALLBACK callback;
	PVOID context;
	WCHAR *outOfProcessCallbackDll;
	size_t references;
	DynamicFunctionTable *next;
};

std::recursive_mutex g_functionTableMutex;
DynamicFunctionTable *g_functionTables = nullptr;

// The registry mutex guards the list's ownership and references held by callback lookups.
void releaseTable(DynamicFunctionTable *table) {
	if (--table->references == 0) {
		std::free(table->outOfProcessCallbackDll);
		std::free(table);
	}
}

class CallbackTableReference {
	DynamicFunctionTable *table;

  public:
	explicit CallbackTableReference(DynamicFunctionTable *value) : table(value) {}
	CallbackTableReference(const CallbackTableReference &) = delete;
	CallbackTableReference &operator=(const CallbackTableReference &) = delete;
	~CallbackTableReference() {
		std::unique_lock lock(g_functionTableMutex);
		releaseTable(table);
	}
};

void appendTable(DynamicFunctionTable *table) {
	auto **tail = &g_functionTables;
	while (*tail)
		tail = &(*tail)->next;
	*tail = table;
}

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
BOOLEAN CDECL RtlInstallFunctionTableCallback(ULONGLONG tableIdentifier, ULONGLONG baseAddress, DWORD length,
											  PGET_RUNTIME_FUNCTION_CALLBACK callback, PVOID context,
											  LPCWSTR outOfProcessCallbackDll) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlInstallFunctionTableCallback(%llx, %llx, %u, %p, %p, %p)\n", tableIdentifier, baseAddress, length,
			  callback, context, outOfProcessCallbackDll);
	if ((tableIdentifier & 3) != 3)
		return FALSE;
	auto *table = static_cast<DynamicFunctionTable *>(std::malloc(sizeof(DynamicFunctionTable)));
	if (!table)
		return FALSE;
	WCHAR *path = nullptr;
	if (outOfProcessCallbackDll) {
		size_t characters = 1;
		for (const WCHAR *character = outOfProcessCallbackDll; *character; ++character)
			++characters;
		path = static_cast<WCHAR *>(std::malloc(characters * sizeof(WCHAR)));
		if (!path) {
			std::free(table);
			return FALSE;
		}
		std::memcpy(path, outOfProcessCallbackDll, characters * sizeof(WCHAR));
	}
	// Retain debugger metadata without loading a library during in-process lookup.
	*table = {nullptr, 0, 0, baseAddress, baseAddress + length, tableIdentifier, callback, context, path, 1, nullptr};
	std::unique_lock lock(g_functionTableMutex);
	appendTable(table);
	return TRUE;
}

BOOLEAN CDECL RtlDeleteFunctionTable(RUNTIME_FUNCTION *functionTable) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlDeleteFunctionTable(%p)\n", functionTable);
	const auto identifier = static_cast<ULONGLONG>(reinterpret_cast<uintptr_t>(functionTable));
	std::unique_lock lock(g_functionTableMutex);
	for (auto **link = &g_functionTables; *link; link = &(*link)->next) {
		if ((*link)->identifier == identifier) {
			auto *table = *link;
			*link = table->next;
			releaseTable(table);
			return TRUE;
		}
	}
	return FALSE;
}

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
	*table = {functionTable,
			  entryCount,
			  maximumEntryCount,
			  rangeBase,
			  rangeEnd,
			  static_cast<ULONGLONG>(reinterpret_cast<uintptr_t>(functionTable)),
			  nullptr,
			  nullptr,
			  nullptr,
			  1,
			  nullptr};
	std::unique_lock lock(g_functionTableMutex);
	appendTable(table);
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
			releaseTable(table);
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
			*imageBase = base;
			return findEntry(image.fromRVA<RUNTIME_FUNCTION>(image.exceptionDirectoryRVA),
							 image.exceptionDirectorySize / sizeof(RUNTIME_FUNCTION), controlPc - base);
		}
	}
	DynamicFunctionTable *callbackTable = nullptr;
	// Serialize callbacks with other registry operations while permitting same-thread reentrancy.
	std::unique_lock lock(g_functionTableMutex);
	for (auto *table = g_functionTables; table; table = table->next) {
		if (controlPc >= table->base && controlPc < table->end) {
			if (table->callback) {
				++table->references;
				callbackTable = table;
				break;
			}
			if (table->entries) {
				auto *entry = findEntry(table->entries, table->count, controlPc - table->base);
				if (entry)
					*imageBase = table->base;
				return entry;
			}
			break;
		}
	}
	if (callbackTable) {
		CallbackTableReference reference(callbackTable);
		// A callback may delete or replace its registration and perform another lookup.
		auto *entry = call_PGET_RUNTIME_FUNCTION_CALLBACK(callbackTable->callback, controlPc, callbackTable->context);
		*imageBase = entry ? callbackTable->base : 0;
		return entry;
	}
	*imageBase = 0;
	return nullptr;
}
} // namespace ntdll
#endif
