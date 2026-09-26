#include "evntprov.h"

#include "common.h"
#include "context.h"
#include "errors.h"

#include <limits>
#include <mutex>
#include <unordered_map>

namespace {

struct EventProvider {
	GUID id;
	PENABLECALLBACK callback;
	PVOID context;
};

std::mutex g_providerMutex;
std::unordered_map<REGHANDLE, EventProvider> g_providers;
REGHANDLE g_nextProviderHandle = 1;

} // namespace

namespace advapi32 {

ULONG WINAPI EventRegister(const GUID *ProviderId, PENABLECALLBACK EnableCallback, PVOID CallbackContext,
						   REGHANDLE *RegHandle) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("EventRegister(%p, %p, %p, %p)\n", ProviderId, EnableCallback, CallbackContext, RegHandle);
	if (!RegHandle) {
		return ERROR_INVALID_PARAMETER;
	}
	*RegHandle = 0;
	if (!ProviderId) {
		return ERROR_INVALID_PARAMETER;
	}
	std::lock_guard lock(g_providerMutex);
	if (g_providers.size() >= 1024 || g_nextProviderHandle == std::numeric_limits<REGHANDLE>::max()) {
		return ERROR_NOT_ENOUGH_MEMORY;
	}
	const REGHANDLE handle = g_nextProviderHandle++;
	// No trace controller enables these process-local registrations.
	g_providers.emplace(handle, EventProvider{*ProviderId, EnableCallback, CallbackContext});
	*RegHandle = handle;
	return ERROR_SUCCESS;
}

ULONG WINAPI EventUnregister(REGHANDLE RegHandle) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("EventUnregister(0x%llx)\n", RegHandle);
	if (!RegHandle) {
		return ERROR_SUCCESS;
	}
	std::lock_guard lock(g_providerMutex);
	return g_providers.erase(RegHandle) ? ERROR_SUCCESS : ERROR_INVALID_HANDLE;
}

} // namespace advapi32
