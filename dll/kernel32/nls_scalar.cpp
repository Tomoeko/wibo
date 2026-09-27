#include "winnls.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "internal.h"
#include "strutil.h"
#include "system_provider.h"

#include <atomic>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr size_t kMaxLocaleNameUnits = 85;
constexpr size_t kMaxClassificationBytes = (64 * 1024 - 256) / 6;
std::atomic<UINT> g_oemCodePage{0};

BOOL failNlsScalar(DWORD error) {
	kernel32::setLastError(error);
	return FALSE;
}

bool readScalarHeader(wibo::provider::Reader &reader) {
	int32_t status = 0;
	if (!reader.header(status)) {
		failNlsScalar(ERROR_INVALID_DATA);
		return false;
	}
	if (!status)
		return true;
	if (!reader.done()) {
		failNlsScalar(ERROR_INVALID_DATA);
		return false;
	}
	failNlsScalar(status == wibo::provider::kUnavailable ? ERROR_NOT_SUPPORTED : static_cast<DWORD>(status));
	return false;
}

bool validByteRange(const void *buffer, size_t bytes) {
	return bytes <= std::numeric_limits<uintptr_t>::max() - reinterpret_cast<uintptr_t>(buffer);
}

} // namespace

namespace kernel32 {

UINT WINAPI GetOEMCP() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetOEMCP()\n");
	const DWORD incomingError = getLastError();
	UINT codePage = g_oemCodePage.load(std::memory_order_acquire);
	if (!codePage) {
		CPINFOEXW information{};
		if (!GetCPInfoExW(1, 0, &information))
			return 0;
		codePage = information.CodePage;
		g_oemCodePage.store(codePage, std::memory_order_release);
	}
	setLastError(incomingError);
	return codePage;
}

BOOL WINAPI IsValidLocaleName(LPCWSTR name) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("IsValidLocaleName(%p)\n", name);
	const DWORD incomingError = getLastError();
	if (!name)
		return FALSE;
	const size_t units = wstrnlen(name, kMaxLocaleNameUnits);
	if (units == kMaxLocaleNameUnits)
		return FALSE;
	const auto encoded =
		wibo::provider::encodeBytes(std::string_view(reinterpret_cast<const char *>(name), units * sizeof(WCHAR)));
	std::vector<uint8_t> response;
	if (!wibo::provider::request({"is-valid-locale-name", encoded, std::to_string(incomingError)}, response))
		return failNlsScalar(ERROR_NOT_SUPPORTED);
	wibo::provider::Reader reader(response);
	if (!readScalarHeader(reader))
		return FALSE;
	uint32_t result = 0, nativeError = 0;
	if (!reader.number(result) || result > 1 || !reader.number(nativeError) || !reader.done())
		return failNlsScalar(ERROR_INVALID_DATA);
	setLastError(nativeError);
	return static_cast<BOOL>(result);
}

BOOL WINAPI GetStringTypeExA(LCID locale, DWORD type, LPCSTR source, int count, LPWORD characterTypes) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetStringTypeExA(%u, %u, %p, %d, %p)\n", locale, type, source, count, characterTypes);
	const DWORD incomingError = getLastError();
	if (!source || !characterTypes || count < -1) {
		// The snapshot transport requires valid buffers and either a count or -1.
		return failNlsScalar(ERROR_NOT_SUPPORTED);
	}
	size_t bytes;
	if (count == -1) {
		if (!validByteRange(source, kMaxClassificationBytes))
			return failNlsScalar(ERROR_INVALID_PARAMETER);
		bytes = strnlen(source, kMaxClassificationBytes);
		if (bytes == kMaxClassificationBytes)
			return failNlsScalar(ERROR_NOT_ENOUGH_MEMORY);
		++bytes;
	} else {
		bytes = static_cast<size_t>(count);
		if (bytes > kMaxClassificationBytes)
			return failNlsScalar(ERROR_NOT_ENOUGH_MEMORY);
	}
	const size_t outputBytes = bytes * sizeof(WORD);
	if (!validByteRange(source, bytes) || !validByteRange(characterTypes, outputBytes))
		return failNlsScalar(ERROR_INVALID_PARAMETER);
	const uintptr_t sourceAddress = reinterpret_cast<uintptr_t>(source);
	const uintptr_t outputAddress = reinterpret_cast<uintptr_t>(characterTypes);
	if (bytes && sourceAddress < outputAddress + outputBytes && outputAddress < sourceAddress + bytes) {
		// Shared subranges need the original buffer identity in the native call.
		return failNlsScalar(ERROR_NOT_SUPPORTED);
	}
	const auto encoded = wibo::provider::encodeBytes(std::string_view(source, bytes));
	const auto initial =
		wibo::provider::encodeBytes(std::string_view(reinterpret_cast<const char *>(characterTypes), outputBytes));
	std::vector<uint8_t> response;
	if (!wibo::provider::request({"string-type-ex-a", std::to_string(locale), std::to_string(type),
								  std::to_string(count), encoded, initial, std::to_string(incomingError)},
								 response))
		return failNlsScalar(ERROR_NOT_SUPPORTED);
	wibo::provider::Reader reader(response);
	if (!readScalarHeader(reader))
		return FALSE;
	uint32_t result = 0, nativeError = 0;
	std::vector<uint8_t> output;
	if (!reader.number(result) || result > 1 || !reader.number(nativeError) || !reader.bytes(output) ||
		!reader.done() || output.size() != outputBytes)
		return failNlsScalar(ERROR_INVALID_DATA);
	if (outputBytes)
		std::memcpy(characterTypes, output.data(), outputBytes);
	setLastError(nativeError);
	return static_cast<BOOL>(result);
}

} // namespace kernel32
