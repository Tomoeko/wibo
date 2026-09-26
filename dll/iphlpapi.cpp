#include "iphlpapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "modules.h"
#include "system_provider.h"

#include <cstring>
#include <vector>

namespace iphlpapi {
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
