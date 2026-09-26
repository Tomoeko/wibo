#include "iphlpapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "modules.h"
#include "system_provider.h"

#include <cstring>
#include <vector>

namespace iphlpapi {
ULONG WINAPI GetBestRoute2(const ULONGLONG *luid, ULONG index, LPCVOID source, LPCVOID destination, ULONG options,
						   LPVOID route, LPVOID bestSource) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetBestRoute2(%p, %u, %p, %p, 0x%x, %p, %p)\n", luid, index, source, destination, options, route,
			  bestSource);
	constexpr size_t addressSize = 28, routeSize = 104;
	if (!destination || !route || !bestSource)
		return ERROR_INVALID_PARAMETER;
	USHORT family = 0;
	std::memcpy(&family, destination, sizeof(family));
	if (family != 2 && family != 23)
		return ERROR_INVALID_PARAMETER;
	const auto encodeAddress = [](LPCVOID address) {
		return address ? wibo::provider::encodeBytes({static_cast<const char *>(address), addressSize}) : std::string();
	};
	std::vector<uint8_t> response;
	if (!wibo::provider::request({"best-route", luid ? std::to_string(*luid) : "none", std::to_string(index),
								  std::to_string(options), encodeAddress(source), encodeAddress(destination)},
								 response))
		return ERROR_NOT_SUPPORTED;
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	if (!reader.header(status))
		return 13;
	if (status)
		return reader.done() ? static_cast<ULONG>(status) : 13;
	std::vector<uint8_t> routeBytes, sourceBytes;
	if (!reader.bytes(routeBytes) || routeBytes.size() != routeSize || !reader.bytes(sourceBytes) ||
		sourceBytes.size() != addressSize || !reader.done())
		return 13;
	std::memcpy(route, routeBytes.data(), routeBytes.size());
	std::memcpy(bestSource, sourceBytes.data(), sourceBytes.size());
	return ERROR_SUCCESS;
}

ULONG WINAPI GetAdaptersAddresses(ULONG family, ULONG flags, LPVOID reserved, LPVOID addresses, ULONG *size) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetAdaptersAddresses(%u, 0x%x, %p, %p, %p)\n", family, flags, reserved, addresses, size);
	if (!size || reserved || (family != 0 && family != 2 && family != 23))
		return ERROR_INVALID_PARAMETER;
	std::vector<uint8_t> response;
	if (!wibo::provider::request(
			{"ip-adapter-addresses", std::to_string(family), std::to_string(flags), std::to_string(sizeof(GUEST_PTR))},
			response))
		return ERROR_NOT_SUPPORTED;
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	if (!reader.header(status))
		return 13;
	if (status == 111) {
		uint32_t required = 0;
		if (!reader.number(required) || !reader.done())
			return 13;
		*size = required;
		return 111;
	}
	if (status)
		return reader.done() ? static_cast<ULONG>(status) : 13;
	uint32_t width = 0, count = 0;
	std::vector<uint8_t> data;
	if (!reader.number(width) || width != sizeof(GUEST_PTR) || !reader.bytes(data) ||
		data.size() < (sizeof(GUEST_PTR) == 8 ? 184 : 144) || !reader.number(count) ||
		count > data.size() / sizeof(GUEST_PTR))
		return 13;
	std::vector<uint32_t> relocations(count);
	uint32_t previous = 0;
	for (uint32_t i = 0; i < count; ++i) {
		auto &offset = relocations[i];
		if (!reader.number(offset) || offset % alignof(GUEST_PTR) || offset > data.size() - sizeof(GUEST_PTR) ||
			(i && offset < previous + sizeof(GUEST_PTR)))
			return 13;
		GUEST_PTR target = 0;
		std::memcpy(&target, data.data() + offset, sizeof(target));
		if (!target || target >= data.size())
			return 13;
		previous = offset;
	}
	if (!reader.done())
		return 13;
	const auto required = static_cast<ULONG>(data.size());
	if (!addresses || *size < required) {
		*size = required;
		return 111; // ERROR_BUFFER_OVERFLOW
	}
	for (const auto offset : relocations) {
		GUEST_PTR target = 0;
		std::memcpy(&target, data.data() + offset, sizeof(target));
		target = toGuestPtr(static_cast<uint8_t *>(addresses) + target);
		std::memcpy(data.data() + offset, &target, sizeof(target));
	}
	std::memcpy(addresses, data.data(), data.size());
	return ERROR_SUCCESS;
}

DWORD WINAPI GetIpAddrTable(LPVOID table, ULONG *size, BOOL ordered) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetIpAddrTable(%p, %p, %u)\n", table, size, ordered);
	if (!size)
		return ERROR_INVALID_PARAMETER;
	std::vector<uint8_t> response;
	if (!wibo::provider::request({"ip-address-table", ordered ? "1" : "0"}, response))
		return ERROR_NOT_SUPPORTED;
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	if (!reader.header(status))
		return 13;
	if (status)
		return reader.done() ? static_cast<DWORD>(status) : 13;
	uint32_t count = 0;
	if (!reader.number(count) || count > 65536)
		return 13;
	std::vector<MIB_IPADDRROW> rows(count);
	for (auto &row : rows) {
		uint32_t type = 0;
		if (!reader.number(row.dwAddr) || !reader.number(row.dwIndex) || !reader.number(row.dwMask) ||
			!reader.number(row.dwBCastAddr) || !reader.number(row.dwReasmSize) || !reader.number(type))
			return 13;
		row.unused = static_cast<WORD>(type);
		row.wType = static_cast<WORD>(type >> 16);
	}
	if (!reader.done())
		return 13;
	const auto required = static_cast<ULONG>(sizeof(DWORD) + rows.size() * sizeof(MIB_IPADDRROW));
	if (!table || *size < required) {
		*size = required;
		return ERROR_INSUFFICIENT_BUFFER;
	}
	std::memcpy(table, &count, sizeof(count));
	if (!rows.empty())
		std::memcpy(static_cast<uint8_t *>(table) + sizeof(count), rows.data(), rows.size() * sizeof(MIB_IPADDRROW));
	return ERROR_SUCCESS;
}
} // namespace iphlpapi

#include "iphlpapi_trampolines.h"

extern const wibo::ModuleStub lib_iphlpapi = {
	(const char *[]){"iphlpapi", nullptr},
	iphlpapiThunkByName,
	nullptr,
};
