#include "user32.h"

#include "context.h"
#include "errors.h"
#include "kernel32/internal.h"
#include "system_provider.h"

#include <bit>

namespace {
constexpr DWORD ERROR_INVALID_DATA = 13;
}

namespace user32 {
int WINAPI GetSystemMetrics(int index) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetSystemMetrics(%d)\n", index);
	std::vector<uint8_t> response;
	if (!wibo::provider::request({"system-metrics", std::to_string(index), std::to_string(kernel32::getLastError())},
								 response)) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return 0;
	}
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	if (!reader.header(status)) {
		kernel32::setLastError(ERROR_INVALID_DATA);
		return 0;
	}
	if (status) {
		kernel32::setLastError(reader.done() ? status : ERROR_INVALID_DATA);
		return 0;
	}
	uint32_t value = 0, error = 0;
	if (!reader.number(value) || !reader.number(error) || !reader.done()) {
		kernel32::setLastError(ERROR_INVALID_DATA);
		return 0;
	}
	kernel32::setLastError(error);
	return std::bit_cast<int32_t>(value);
}
} // namespace user32
