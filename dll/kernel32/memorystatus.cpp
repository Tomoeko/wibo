#include "sysinfoapi.h"

#include "context.h"
#include "errors.h"
#include "internal.h"
#include "memoryapi.h"
#include "system_provider.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/resource.h>

namespace {

bool availableVirtualMemory(uint64_t &total, uint64_t &available) {
	SYSTEM_INFO system{};
	kernel32::GetSystemInfo(&system);
	const uint64_t minimum = system.lpMinimumApplicationAddress;
	const uint64_t limit = uint64_t(system.lpMaximumApplicationAddress) + 1;
	total = limit - minimum;
	available = 0;
	uint64_t cursor = minimum;
	while (cursor < limit) {
		MEMORY_BASIC_INFORMATION region{};
		if (!kernel32::VirtualQuery(reinterpret_cast<void *>(uintptr_t(cursor)), &region, sizeof(region)))
			return false;
		const uint64_t base = region.BaseAddress;
		if (base > cursor || region.RegionSize > UINT64_MAX - base || base + region.RegionSize <= cursor) {
			kernel32::setLastError(ERROR_INVALID_DATA);
			return false;
		}
		const uint64_t end = std::min(limit, base + region.RegionSize);
		if (region.State == MEM_FREE)
			available += end - cursor;
		cursor = end;
	}
	return true;
}
} // namespace

namespace kernel32 {

BOOL WINAPI GlobalMemoryStatusEx(MEMORYSTATUSEX *status) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GlobalMemoryStatusEx(%p)\n", status);
	if (!status || status->dwLength != sizeof(*status)) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	std::vector<uint8_t> response;
	if (!wibo::provider::request({"memory-status"}, response)) {
		setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	wibo::provider::Reader reader(response);
	int32_t error = 0;
	if (!reader.header(error)) {
		setLastError(ERROR_INVALID_DATA);
		return FALSE;
	}
	if (error) {
		setLastError(reader.done() ? error : ERROR_INVALID_DATA);
		return FALSE;
	}
	std::vector<uint8_t> data;
	if (!reader.bytes(data) || data.size() != sizeof(*status) || !reader.done()) {
		setLastError(ERROR_INVALID_DATA);
		return FALSE;
	}
	MEMORYSTATUSEX result{};
	std::memcpy(&result, data.data(), sizeof(result));
	if (result.dwLength != sizeof(result) || result.dwMemoryLoad > 100 || !result.ullTotalPhys ||
		result.ullAvailPhys > result.ullTotalPhys || !result.ullTotalPageFile ||
		result.ullAvailPageFile > result.ullTotalPageFile || result.ullAvailExtendedVirtual) {
		setLastError(ERROR_INVALID_DATA);
		return FALSE;
	}
	// Virtual availability belongs to this process, rather than the adapter.
	uint64_t total = 0, available = 0;
	if (!availableVirtualMemory(total, available))
		return FALSE;
	result.ullTotalVirtual = total;
	result.ullAvailVirtual = available;
	rlimit addressLimit{};
	if (getrlimit(RLIMIT_AS, &addressLimit) != 0) {
		setLastError(wibo::winErrorFromErrno(errno));
		return FALSE;
	}
	if (addressLimit.rlim_cur != RLIM_INFINITY) {
		const uint64_t used = total - available;
		const uint64_t cap = addressLimit.rlim_cur;
		result.ullTotalPageFile = std::min<uint64_t>(result.ullTotalPageFile, cap);
		result.ullAvailPageFile = std::min<uint64_t>(result.ullAvailPageFile, used < cap ? cap - used : 0);
	}
	*status = result;
	return TRUE;
}

} // namespace kernel32
