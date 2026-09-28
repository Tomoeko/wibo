#include "user32.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "kernel32/internal.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

constexpr BYTE FVIRTKEY = 0x01;

struct AcceleratorRegistry {
	std::mutex mutex;
	std::unordered_map<HANDLE, std::vector<user32::ACCEL>> tables;
	// Accelerator IDs remain distinct from the aligned menu and kernel handle ranges.
	HANDLE nextHandle = 0x10001;
};

AcceleratorRegistry &registry() {
	static AcceleratorRegistry value;
	return value;
}

HANDLE createTable(const user32::ACCEL *entries, int count, bool ansi) {
	if (!entries || count < 1 || count > 32767) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	std::vector<user32::ACCEL> copy;
	copy.reserve(static_cast<size_t>(count));
	for (int i = 0; i < count; ++i) {
		user32::ACCEL entry{};
		entry.fVirt = entries[i].fVirt;
		entry.key = entries[i].key;
		entry.cmd = entries[i].cmd;
		if (ansi && !(entry.fVirt & FVIRTKEY)) {
			// An ANSI character key requires the caller's native code page.
			const BYTE character = static_cast<BYTE>(entry.key);
			if (character > 0x7f) {
				kernel32::setLastError(ERROR_NOT_SUPPORTED);
				return 0;
			}
			entry.key = character;
		}
		copy.push_back(entry);
	}
	AcceleratorRegistry &state = registry();
	std::lock_guard lock(state.mutex);
	if (state.nextHandle > std::numeric_limits<HANDLE>::max() - 4) {
		kernel32::setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return 0;
	}
	const HANDLE handle = state.nextHandle;
	state.nextHandle += 4;
	state.tables.emplace(handle, std::move(copy));
	return handle;
}

int copyTable(HANDLE table, user32::ACCEL *entries, int count, bool ansi) {
	AcceleratorRegistry &state = registry();
	std::lock_guard lock(state.mutex);
	const auto found = state.tables.find(table);
	if (found == state.tables.end())
		return 0;
	const auto &source = found->second;
	if (!entries)
		return static_cast<int>(source.size());
	if (count < 0) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	const int copied = std::min(count, static_cast<int>(source.size()));
	for (int i = 0; i < copied; ++i) {
		if (ansi && !(source[i].fVirt & FVIRTKEY) && source[i].key > 0x7f) {
			kernel32::setLastError(ERROR_NOT_SUPPORTED);
			return 0;
		}
	}
	for (int i = 0; i < copied; ++i) {
		entries[i].fVirt = source[i].fVirt & 0x7f;
		entries[i].key = source[i].key;
		entries[i].cmd = source[i].cmd;
	}
	return copied;
}

} // namespace

namespace user32 {

HANDLE WINAPI CreateAcceleratorTableA(const ACCEL *entries, int count) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CreateAcceleratorTableA(%p, %d)\n", entries, count);
	return createTable(entries, count, true);
}

HANDLE WINAPI CreateAcceleratorTableW(const ACCEL *entries, int count) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CreateAcceleratorTableW(%p, %d)\n", entries, count);
	return createTable(entries, count, false);
}

BOOL WINAPI DestroyAcceleratorTable(HANDLE table) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("DestroyAcceleratorTable(%lld)\n", static_cast<long long>(table));
	AcceleratorRegistry &state = registry();
	std::lock_guard lock(state.mutex);
	return state.tables.erase(table) ? TRUE : FALSE;
}

int WINAPI CopyAcceleratorTableA(HANDLE table, ACCEL *entries, int count) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CopyAcceleratorTableA(%lld, %p, %d)\n", static_cast<long long>(table), entries, count);
	return copyTable(table, entries, count, true);
}

int WINAPI CopyAcceleratorTableW(HANDLE table, ACCEL *entries, int count) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CopyAcceleratorTableW(%lld, %p, %d)\n", static_cast<long long>(table), entries, count);
	return copyTable(table, entries, count, false);
}

int WINAPI TranslateAcceleratorA(HWND window, HANDLE table, const MSG *message) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("TranslateAcceleratorA(%lld, %lld, %p)\n", static_cast<long long>(window), static_cast<long long>(table),
			  message);
	(void)table;
	(void)message;
	if (window)
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
	return 0;
}

int WINAPI TranslateAcceleratorW(HWND window, HANDLE table, const MSG *message) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("TranslateAcceleratorW(%lld, %lld, %p)\n", static_cast<long long>(window), static_cast<long long>(table),
			  message);
	(void)table;
	(void)message;
	if (window)
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
	return 0;
}

} // namespace user32
