#include "user32.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "kernel32/internal.h"
#include "system_provider.h"

#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr DWORD kMaxUpperBufferUnits = 16000;

DWORD failUpperBuffer(DWORD error) {
	kernel32::setLastError(error);
	return 0;
}

} // namespace

namespace user32 {

DWORD WINAPI CharUpperBuffW(LPWSTR buffer, DWORD length) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CharUpperBuffW(%p, %u)\n", buffer, length);
	const DWORD incomingError = kernel32::getLastError();
	if (!buffer)
		return 0;
	if (!length)
		return failUpperBuffer(ERROR_INVALID_PARAMETER);
	const size_t byteCount = static_cast<size_t>(length) * sizeof(WCHAR);
	if (byteCount / sizeof(WCHAR) != length ||
		byteCount > std::numeric_limits<uintptr_t>::max() - reinterpret_cast<uintptr_t>(buffer))
		return failUpperBuffer(ERROR_INVALID_PARAMETER);
	bool ascii = true;
	for (DWORD index = 0; index < length; ++index) {
		if (buffer[index] >= 128) {
			ascii = false;
			break;
		}
	}
	if (ascii) {
		for (DWORD index = 0; index < length; ++index) {
			if (buffer[index] >= 'a' && buffer[index] <= 'z')
				buffer[index] -= 'a' - 'A';
		}
		kernel32::setLastError(incomingError);
		return length;
	}
	// Keep the complete counted buffer in one request, including surrogate pairs.
	if (length > kMaxUpperBufferUnits)
		return failUpperBuffer(ERROR_NOT_SUPPORTED);
	const auto encoded =
		wibo::provider::encodeBytes(std::string_view(reinterpret_cast<const char *>(buffer), byteCount));
	std::vector<uint8_t> response;
	if (!wibo::provider::request({"char-upper-buff-w", std::to_string(length), encoded, std::to_string(incomingError)},
								 response))
		return failUpperBuffer(ERROR_NOT_SUPPORTED);
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	if (!reader.header(status))
		return failUpperBuffer(ERROR_INVALID_DATA);
	if (status) {
		if (!reader.done())
			return failUpperBuffer(ERROR_INVALID_DATA);
		return failUpperBuffer(status == wibo::provider::kUnavailable ? ERROR_NOT_SUPPORTED
																	  : static_cast<DWORD>(status));
	}
	uint32_t result = 0, nativeError = 0;
	std::vector<uint8_t> output;
	if (!reader.number(result) || result > length || !reader.number(nativeError) || !reader.bytes(output) ||
		!reader.done() || output.size() != byteCount)
		return failUpperBuffer(ERROR_INVALID_DATA);
	std::memcpy(buffer, output.data(), byteCount);
	kernel32::setLastError(nativeError);
	return result;
}

} // namespace user32
