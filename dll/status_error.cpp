#include "ntdll.h"

#include "context.h"
#include "errors.h"
#include "kernel32/internal.h"
#include "system_provider.h"

#include <mutex>
#include <unordered_map>

namespace {
constexpr ULONG kUnmappedStatus = 317;
constexpr DWORD kInvalidData = 13;
constexpr size_t kMaxCachedStatuses = 4096;
std::mutex g_statusMutex;
std::unordered_map<uint32_t, ULONG> g_statusErrors;
} // namespace

namespace ntdll {
ULONG WINAPI RtlNtStatusToDosError(NTSTATUS status) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlNtStatusToDosError(0x%x)\n", static_cast<uint32_t>(status));
	const auto key = static_cast<uint32_t>(status);
	// Status mappings are immutable within the configured execution environment.
	std::lock_guard lock(g_statusMutex);
	if (const auto found = g_statusErrors.find(key); found != g_statusErrors.end())
		return found->second;
	std::vector<uint8_t> response;
	if (!wibo::provider::request({"status-error", std::to_string(key)}, response)) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return kUnmappedStatus;
	}
	wibo::provider::Reader reader(response);
	int32_t error = 0;
	if (!reader.header(error)) {
		kernel32::setLastError(kInvalidData);
		return kUnmappedStatus;
	}
	if (error) {
		kernel32::setLastError(reader.done() ? error : kInvalidData);
		return kUnmappedStatus;
	}
	uint32_t result = 0;
	if (!reader.number(result) || !reader.done()) {
		kernel32::setLastError(kInvalidData);
		return kUnmappedStatus;
	}
	if (g_statusErrors.size() < kMaxCachedStatuses)
		g_statusErrors.emplace(key, result);
	return result;
}
} // namespace ntdll
