#include "evntprov.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "strutil.h"

#include <cstring>
#include <limits>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace {

struct EventProvider {
	GUID id;
	PENABLECALLBACK callback;
	PVOID context;
	bool useDescriptorType = false;
};

std::mutex g_providerMutex;
std::unordered_map<REGHANDLE, EventProvider> g_providers;
REGHANDLE g_nextProviderHandle = 1;

bool validProviderTraits(const void *information, ULONG length) {
	// The size fields include themselves and may be unaligned in the blob.
	if (!information || length < 3 || length > std::numeric_limits<WORD>::max()) {
		return false;
	}
	const auto *bytes = static_cast<const unsigned char *>(information);
	const auto readSize = [](const unsigned char *value) { return static_cast<WORD>(value[0] | (value[1] << 8)); };
	if (readSize(bytes) != length) {
		return false;
	}
	const auto *name = bytes + sizeof(WORD);
	const auto *nameEnd = static_cast<const unsigned char *>(std::memchr(name, 0, length - sizeof(WORD)));
	if (!nameEnd || !utf8ToUtf16(std::string_view(reinterpret_cast<const char *>(name), nameEnd - name))) {
		return false;
	}
	size_t offset = static_cast<size_t>(nameEnd - bytes) + 1;
	while (offset < length) {
		if (length - offset < 3) {
			return false;
		}
		const WORD traitSize = readSize(bytes + offset);
		if (traitSize < 3 || traitSize > length - offset) {
			return false;
		}
		offset += traitSize;
	}
	return true;
}

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

ULONG WINAPI EventSetInformation(REGHANDLE RegHandle, EVENT_INFO_CLASS InformationClass, PVOID EventInformation,
								 ULONG InformationLength) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("EventSetInformation(0x%llx, %u, %p, %u)\n", RegHandle, static_cast<unsigned>(InformationClass),
			  EventInformation, InformationLength);
	std::lock_guard lock(g_providerMutex);
	const auto provider = g_providers.find(RegHandle);
	if (provider == g_providers.end() || (!EventInformation && InformationLength)) {
		return ERROR_INVALID_PARAMETER;
	}
	switch (InformationClass) {
	case EventProviderUseDescriptorType: {
		if (!EventInformation || InformationLength != 1) {
			return ERROR_INVALID_PARAMETER;
		}
		const auto enabled = *static_cast<const unsigned char *>(EventInformation);
		if (enabled > 1) {
			return ERROR_INVALID_PARAMETER;
		}
		provider->second.useDescriptorType = enabled != 0;
		return ERROR_SUCCESS;
	}
	case EventProviderSetTraits:
		if (!validProviderTraits(EventInformation, InformationLength)) {
			return ERROR_INVALID_PARAMETER;
		}
		// Trait storage, provider groups, and trace session delivery are unavailable.
		return ERROR_NOT_SUPPORTED;
	case EventProviderBinaryTrackInfo:
	default:
		// Binary tracking ignores the buffer; callback module tracking and other operations are unavailable.
		return ERROR_NOT_SUPPORTED;
	}
}

} // namespace advapi32
