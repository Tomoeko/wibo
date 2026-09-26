#include "network_proxy.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "heap.h"
#include "network_proxy_trampolines.h"
#include "system_provider.h"

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {
constexpr HRESULT kPointer = static_cast<HRESULT>(0x80004003);
constexpr HRESULT kNoInterface = static_cast<HRESULT>(0x80004002);
constexpr HRESULT kUnavailable = static_cast<HRESULT>(0x80004001);
constexpr HRESULT kBadResponse = static_cast<HRESULT>(0x8007000D);
constexpr GUID kClassManager{0xDCB00C01, 0x570F, 0x4A9B, {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};
constexpr GUID kManager{0xDCB00000, 0x570F, 0x4A9B, {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};
constexpr GUID kUnknown{0, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr GUID kDispatch{0x20400, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};

struct State {
	ULONG references;
	DWORD context;
};
struct Objects {
	std::mutex mutex;
	std::unordered_map<GUEST_PTR, State> objects;
	GUEST_PTR table = 0;
	~Objects() {
		for (const auto &[self, state] : objects)
			wibo::heap::guestFree(fromGuestPtr(self));
		wibo::heap::guestFree(fromGuestPtr(table));
	}
};
Objects &objects() {
	static Objects registry;
	return registry;
}
bool sameGuid(const GUID *a, const GUID &b) { return a && std::memcmp(a, &b, sizeof(b)) == 0; }
bool supported(const GUID *iid) {
	return sameGuid(iid, kManager) || sameGuid(iid, kUnknown) || sameGuid(iid, kDispatch);
}

struct Connectivity {
	uint32_t flags = 0, connected = 0, internet = 0;
};
HRESULT query(DWORD context, Connectivity &result) {
	std::vector<uint8_t> response;
	if (!wibo::provider::request({"network-connectivity", std::to_string(context)}, response))
		return kUnavailable;
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	if (!reader.header(status))
		return kBadResponse;
	if (status < 0)
		return reader.done() ? status : kBadResponse;
	if (!reader.number(result.flags) || !reader.number(result.connected) || !reader.number(result.internet) ||
		!reader.done() || (result.flags & ~0x773U) || result.connected > 1 || result.internet > 1)
		return kBadResponse;
	return status;
}
HRESULT queryObject(GUEST_PTR self, Connectivity &result) {
	auto &registry = objects();
	DWORD context;
	{
		std::lock_guard lock(registry.mutex);
		const auto found = registry.objects.find(self);
		if (found == registry.objects.end())
			return kPointer;
		context = found->second.context;
	}
	return query(context, result);
}
GUEST_PTR makeTable() {
	constexpr const char *names[] = {"QueryInterface",	"AddRef",		   "Release",		  "Unavailable2",
									 "Unavailable4",	"Unavailable6",	   "Unavailable9",	  "Unavailable3",
									 "UnavailableGuid", "Unavailable2",	   "UnavailableGuid", "IsConnectedToInternet",
									 "IsConnected",		"GetConnectivity", "Unavailable2",	  "Unavailable1"};
	auto *table =
		static_cast<GUEST_PTR *>(wibo::heap::guestMalloc(sizeof(names) / sizeof(names[0]) * sizeof(GUEST_PTR)));
	if (!table)
		return 0;
	for (size_t index = 0; index < sizeof(names) / sizeof(names[0]); ++index)
		table[index] = toGuestPtr(network_proxyThunkByName(names[index]));
	return toGuestPtr(table);
}
} // namespace

namespace wibo::network {
bool isManagerClass(const GUID *classId) { return sameGuid(classId, kClassManager); }
HRESULT createManager(LPVOID outer, DWORD context, const GUID *iid, GUEST_PTR *result) {
	if (!result)
		return kPointer;
	*result = 0;
	if (!iid)
		return kPointer;
	if (outer)
		return static_cast<HRESULT>(0x80040110);
	if (!supported(iid))
		return kNoInterface;
	if (!(context & 5) || !provider::configured())
		return static_cast<HRESULT>(0x80040154);
	Connectivity value;
	const HRESULT status = query(context, value);
	if (status < 0)
		return status;
	auto &registry = objects();
	std::lock_guard lock(registry.mutex);
	if (!registry.table)
		registry.table = makeTable();
	if (!registry.table)
		return static_cast<HRESULT>(0x8007000E);
	auto *self = static_cast<GUEST_PTR *>(heap::guestMalloc(sizeof(GUEST_PTR)));
	if (!self)
		return static_cast<HRESULT>(0x8007000E);
	*self = registry.table;
	*result = toGuestPtr(self);
	registry.objects.emplace(*result, State{1, context});
	return S_OK;
}
} // namespace wibo::network

namespace network_proxy {
HRESULT WINAPI QueryInterface(GUEST_PTR self, const GUID *iid, GUEST_PTR *result) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("INetworkListManager::QueryInterface(%p, %p, %p)\n", fromGuestPtr(self), iid, result);
	if (!result)
		return kPointer;
	*result = 0;
	auto &registry = objects();
	std::lock_guard lock(registry.mutex);
	const auto found = registry.objects.find(self);
	if (found == registry.objects.end() || !iid)
		return kPointer;
	if (!supported(iid))
		return kNoInterface;
	++found->second.references;
	*result = self;
	return S_OK;
}
ULONG WINAPI AddRef(GUEST_PTR self) {
	HOST_CONTEXT_GUARD();
	auto &registry = objects();
	std::lock_guard lock(registry.mutex);
	const auto found = registry.objects.find(self);
	return found == registry.objects.end() ? 0 : ++found->second.references;
}
ULONG WINAPI Release(GUEST_PTR self) {
	HOST_CONTEXT_GUARD();
	auto &registry = objects();
	std::lock_guard lock(registry.mutex);
	const auto found = registry.objects.find(self);
	if (found == registry.objects.end())
		return 0;
	const ULONG remaining = --found->second.references;
	if (!remaining) {
		registry.objects.erase(found);
		wibo::heap::guestFree(fromGuestPtr(self));
	}
	return remaining;
}
HRESULT WINAPI GetConnectivity(GUEST_PTR self, DWORD *result) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("INetworkListManager::GetConnectivity(%p, %p)\n", fromGuestPtr(self), result);
	if (!result)
		return kPointer;
	Connectivity value;
	const HRESULT status = queryObject(self, value);
	if (status >= 0)
		*result = value.flags;
	return status;
}
HRESULT WINAPI IsConnected(GUEST_PTR self, SHORT *result) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("INetworkListManager::IsConnected(%p, %p)\n", fromGuestPtr(self), result);
	if (!result)
		return kPointer;
	Connectivity value;
	const HRESULT status = queryObject(self, value);
	if (status >= 0)
		*result = value.connected ? -1 : 0;
	return status;
}
HRESULT WINAPI IsConnectedToInternet(GUEST_PTR self, SHORT *result) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("INetworkListManager::IsConnectedToInternet(%p, %p)\n", fromGuestPtr(self), result);
	if (!result)
		return kPointer;
	Connectivity value;
	const HRESULT status = queryObject(self, value);
	if (status >= 0)
		*result = value.internet ? -1 : 0;
	return status;
}
HRESULT WINAPI Unavailable1(GUEST_PTR) {
	HOST_CONTEXT_GUARD();
	return kUnavailable;
}
HRESULT WINAPI Unavailable2(GUEST_PTR, GUEST_PTR) {
	HOST_CONTEXT_GUARD();
	return kUnavailable;
}
HRESULT WINAPI Unavailable3(GUEST_PTR, GUEST_PTR, GUEST_PTR) {
	HOST_CONTEXT_GUARD();
	return kUnavailable;
}
HRESULT WINAPI Unavailable4(GUEST_PTR, GUEST_PTR, GUEST_PTR, GUEST_PTR) {
	HOST_CONTEXT_GUARD();
	return kUnavailable;
}
HRESULT WINAPI Unavailable6(GUEST_PTR, GUEST_PTR, GUEST_PTR, GUEST_PTR, GUEST_PTR, GUEST_PTR) {
	HOST_CONTEXT_GUARD();
	return kUnavailable;
}
HRESULT WINAPI Unavailable9(GUEST_PTR, GUEST_PTR, GUEST_PTR, GUEST_PTR, GUEST_PTR, GUEST_PTR, GUEST_PTR, GUEST_PTR,
							GUEST_PTR) {
	HOST_CONTEXT_GUARD();
	return kUnavailable;
}
#ifdef WIBO_GUEST_64
HRESULT WINAPI UnavailableGuid(GUEST_PTR, GUEST_PTR, GUEST_PTR) {
	HOST_CONTEXT_GUARD();
	return kUnavailable;
}
#else
HRESULT WINAPI UnavailableGuid(GUEST_PTR, DWORD, DWORD, DWORD, DWORD, GUEST_PTR) {
	HOST_CONTEXT_GUARD();
	return kUnavailable;
}
#endif
} // namespace network_proxy
