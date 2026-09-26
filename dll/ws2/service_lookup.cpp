#include "ws2.h"

#include "common.h"
#include "context.h"
#include "handles.h"
#include "strutil.h"
#include "ws2/internal.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

namespace {
constexpr DWORD kReturnName = 0x10;
constexpr DWORD kReturnType = 0x20;
constexpr DWORD kFlushPrevious = 0x2000;
constexpr DWORD kDnsNamespace = 12;
constexpr GUID kHostNameClass = {0x0002a800, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};

struct ServiceLookup final : ObjectBase {
	static constexpr ObjectType kType = ObjectType::ServiceLookup;
	std::mutex mutex;
	DWORD fields;
	std::vector<WCHAR> name;
	bool consumed = false;
	bool closed = false;
	ServiceLookup(DWORD fields, std::vector<WCHAR> name) : ObjectBase(kType), fields(fields), name(std::move(name)) {}
};

int fail(int error) {
	ws2::detail::setLastError(error);
	return -1;
}
} // namespace

namespace ws2 {
int WINAPI WSALookupServiceBeginW(const WSAQUERYSETW *restrictions, DWORD flags, LPHANDLE lookup) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("WSALookupServiceBeginW(%p, 0x%x, %p)\n", restrictions, flags, lookup);
	if (!detail::requireStarted())
		return -1;
	if (!restrictions || !lookup)
		return fail(10014);
	if (restrictions->dwSize != sizeof(WSAQUERYSETW) || !restrictions->lpServiceClassId)
		return fail(10022);
	if (std::memcmp(fromGuestPtr(restrictions->lpServiceClassId), &kHostNameClass, sizeof(GUID)) != 0)
		return fail(10045); // Unimplemented service classes require another namespace backend.
	if ((flags & ~(kReturnName | kReturnType)) ||
		(restrictions->dwNameSpace != 0 && restrictions->dwNameSpace != kDnsNamespace) ||
		restrictions->lpNSProviderId || restrictions->lpVersion || restrictions->lpszContext ||
		restrictions->lpszQueryString || restrictions->lpBlob)
		return fail(10045);
	if (restrictions->lpszServiceInstanceName && *fromGuestPtr<WCHAR>(restrictions->lpszServiceInstanceName))
		return fail(10045); // This query resolves the local host only.
	if (restrictions->dwNumberOfProtocols > 64)
		return fail(10022);
	if (restrictions->dwNumberOfProtocols) {
		if (!restrictions->lpafpProtocols)
			return fail(10014);
		const auto *protocols = fromGuestPtr<AFPROTOCOLS>(restrictions->lpafpProtocols);
		bool supported = false;
		for (DWORD i = 0; i < restrictions->dwNumberOfProtocols; ++i) {
			const auto &protocol = protocols[i];
			if ((protocol.iAddressFamily == 0 || protocol.iAddressFamily == 2 || protocol.iAddressFamily == 23) &&
				(protocol.iProtocol == 0 || protocol.iProtocol == 6 || protocol.iProtocol == 17))
				supported = true;
		}
		if (!supported)
			return fail(11004); // WSANO_DATA
	}
	std::string host;
	if (!detail::localHostName(host))
		return -1;
	if (std::any_of(host.begin(), host.end(), [](unsigned char byte) { return byte >= 128; }))
		return fail(10045);
	auto query = make_pin<ServiceLookup>(flags, stringToWideString(host.c_str()));
	const HANDLE handle = wibo::handles().alloc(std::move(query), 0, 0);
	if (handle == NO_HANDLE || handle == static_cast<HANDLE>(-1))
		return fail(8);
	*lookup = handle;
	return 0;
}

int WINAPI WSALookupServiceNextW(HANDLE lookup, DWORD flags, LPDWORD length, WSAQUERYSETW *result) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("WSALookupServiceNextW(%p, 0x%x, %p, %p)\n", lookup, flags, length, result);
	if (!detail::requireStarted())
		return -1;
	auto query = wibo::handles().getAs<ServiceLookup>(lookup);
	if (!query)
		return fail(6); // WSA_INVALID_HANDLE
	if (!length)
		return fail(10014);
	if (flags & ~(kReturnName | kReturnType | kFlushPrevious))
		return fail(10045);
	std::lock_guard guard(query->mutex);
	if (query->closed)
		return fail(10111); // WSA_E_CANCELLED
	if (flags & kFlushPrevious)
		query->consumed = true;
	if (query->consumed)
		return fail(10110); // WSA_E_NO_MORE
	const DWORD fields = query->fields & ((flags & (kReturnName | kReturnType)) ? flags : query->fields);
	const size_t nameBytes = (fields & kReturnName) ? query->name.size() * sizeof(WCHAR) : 0;
	const size_t typeBytes = (fields & kReturnType) ? sizeof(GUID) : 0;
	const DWORD required = static_cast<DWORD>(sizeof(WSAQUERYSETW) + typeBytes + nameBytes);
	if (!result || *length < required) {
		*length = required;
		return fail(10014);
	}
	WSAQUERYSETW output{};
	output.dwSize = sizeof(output);
	output.dwNameSpace = kDnsNamespace;
	auto *cursor = reinterpret_cast<BYTE *>(result) + sizeof(output);
	if (typeBytes) {
		output.lpServiceClassId = toGuestPtr(cursor);
		std::memcpy(cursor, &kHostNameClass, typeBytes);
		cursor += typeBytes;
	}
	if (nameBytes) {
		output.lpszServiceInstanceName = toGuestPtr(cursor);
		std::memcpy(cursor, query->name.data(), nameBytes);
	}
	std::memcpy(result, &output, sizeof(output));
	query->consumed = true;
	return 0;
}

int WINAPI WSALookupServiceEnd(HANDLE lookup) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("WSALookupServiceEnd(%p)\n", lookup);
	if (!detail::requireStarted())
		return -1;
	auto query = wibo::handles().getAs<ServiceLookup>(lookup);
	if (!query)
		return fail(6);
	std::lock_guard guard(query->mutex);
	if (query->closed)
		return fail(6);
	query->closed = true;
	return wibo::handles().release(lookup) ? 0 : fail(6);
}
} // namespace ws2
