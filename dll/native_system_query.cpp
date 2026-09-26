#include "ntdll.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "system_provider.h"

#include <cstring>

namespace ntdll {
NTSTATUS WINAPI NtQuerySystemInformation(ULONG informationClass, PVOID information, ULONG length,
										 PULONG returnedLength) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("NtQuerySystemInformation(%u, %p, %u, %p)\n", informationClass, information, length, returnedLength);
	// Clock and processor counters have no embedded pointers in either guest ABI.
	if ((informationClass != 3 && informationClass != 8) || length > 1024 * 1024)
		return STATUS_NOT_SUPPORTED;
	if (!information && (length || informationClass == 3)) {
		if (returnedLength && informationClass == 3)
			*returnedLength = 0;
		return STATUS_ACCESS_VIOLATION;
	}
	std::vector<uint8_t> response;
	if (!wibo::provider::request({"system-query", std::to_string(informationClass), std::to_string(length)}, response))
		return STATUS_NOT_SUPPORTED;
	wibo::provider::Reader reader(response);
	int32_t error = 0;
	if (!reader.header(error))
		return STATUS_UNEXPECTED_IO_ERROR;
	if (error)
		return reader.done() ? wibo::statusFromWinError(error) : STATUS_UNEXPECTED_IO_ERROR;
	uint32_t status = 0, returned = 0;
	std::vector<uint8_t> data;
	if (!reader.number(status) || !reader.number(returned) || !reader.bytes(data) || !reader.done())
		return STATUS_UNEXPECTED_IO_ERROR;
	const NTSTATUS result = static_cast<NTSTATUS>(status);
	if ((result >= 0 && (returned > length || data.size() != returned)) || (result < 0 && !data.empty()))
		return STATUS_UNEXPECTED_IO_ERROR;
	if (!data.empty())
		memcpy(information, data.data(), data.size());
	if (returnedLength)
		*returnedLength = returned;
	return result;
}
} // namespace ntdll
