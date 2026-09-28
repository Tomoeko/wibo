#include "user32.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "kernel32/internal.h"
#include "kernel32/processthreadsapi.h"
#include "system_provider.h"

#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr size_t kLayoutNameUnits = 9;
constexpr int kMaxUnicodeOutputUnits = 256;
constexpr size_t kMaxIssuedLayouts = 16;
std::mutex g_issuedLayoutsMutex;
std::array<HKL, kMaxIssuedLayouts> g_issuedLayouts{};
size_t g_issuedLayoutCount = 0;

bool recordIssuedLayout(HKL layout) {
	std::lock_guard lock(g_issuedLayoutsMutex);
	for (size_t index = 0; index < g_issuedLayoutCount; ++index)
		if (g_issuedLayouts[index] == layout)
			return true;
	if (g_issuedLayoutCount == g_issuedLayouts.size())
		return false;
	g_issuedLayouts[g_issuedLayoutCount++] = layout;
	return true;
}

bool wasIssuedLayout(HKL layout) {
	std::lock_guard lock(g_issuedLayoutsMutex);
	for (size_t index = 0; index < g_issuedLayoutCount; ++index)
		if (g_issuedLayouts[index] == layout)
			return true;
	return false;
}

DWORD providerError(int32_t status) {
	if (status == wibo::provider::kUnavailable)
		return ERROR_NOT_SUPPORTED;
	if (status <= 0)
		return ERROR_INVALID_DATA;
	return static_cast<DWORD>(status);
}

bool failKeyboard(DWORD error) {
	kernel32::setLastError(error);
	return false;
}

bool readLayout(HKL &layout, std::array<WCHAR, kLayoutNameUnits> *name = nullptr) {
	std::vector<uint8_t> response;
	if (!wibo::provider::request({"keyboard-layout"}, response))
		return failKeyboard(ERROR_NOT_SUPPORTED);
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	if (!reader.header(status))
		return failKeyboard(ERROR_INVALID_DATA);
	if (status) {
		if (!reader.done())
			return failKeyboard(ERROR_INVALID_DATA);
		return failKeyboard(providerError(status));
	}
	uint32_t low = 0, high = 0;
	std::vector<uint8_t> rawName;
	if (!reader.number(low) || !reader.number(high) || !reader.bytes(rawName) || !reader.done() ||
		rawName.size() != kLayoutNameUnits * sizeof(WCHAR))
		return failKeyboard(ERROR_INVALID_DATA);
	const uint64_t value = (uint64_t(high) << 32) | low;
	if (!value || value > std::numeric_limits<GUEST_PTR>::max())
		return failKeyboard(ERROR_NOT_SUPPORTED);
	std::array<WCHAR, kLayoutNameUnits> parsedName{};
	std::memcpy(parsedName.data(), rawName.data(), rawName.size());
	for (size_t index = 0; index + 1 < parsedName.size(); ++index) {
		const WCHAR character = parsedName[index];
		if (!((character >= '0' && character <= '9') || (character >= 'A' && character <= 'F') ||
			  (character >= 'a' && character <= 'f')))
			return failKeyboard(ERROR_INVALID_DATA);
	}
	if (parsedName.back())
		return failKeyboard(ERROR_INVALID_DATA);
	layout = static_cast<HKL>(value);
	if (!recordIssuedLayout(layout))
		return failKeyboard(ERROR_NOT_SUPPORTED);
	if (name)
		*name = parsedName;
	return true;
}

bool readScalarResult(const std::vector<std::string> &request, uint32_t &result) {
	std::vector<uint8_t> response;
	if (!wibo::provider::request(request, response))
		return failKeyboard(ERROR_NOT_SUPPORTED);
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	if (!reader.header(status))
		return failKeyboard(ERROR_INVALID_DATA);
	if (status) {
		if (!reader.done())
			return failKeyboard(ERROR_INVALID_DATA);
		return failKeyboard(providerError(status));
	}
	uint32_t nativeError = 0;
	if (!reader.number(result) || !reader.number(nativeError) || !reader.done())
		return failKeyboard(ERROR_INVALID_DATA);
	kernel32::setLastError(nativeError);
	return true;
}

UINT mapVirtualKey(UINT code, UINT type, const char *variant, HKL requested) {
	const DWORD incomingError = kernel32::getLastError();
	const bool explicitLayout = std::string_view(variant) == "ex-a";
	HKL layout = requested;
	if (explicitLayout && !wasIssuedLayout(layout)) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return 0;
	}
	if (!explicitLayout && !readLayout(layout))
		return 0;
	const uint64_t value = static_cast<GUEST_PTR>(layout);
	uint32_t result = 0;
	if (!readScalarResult({"keyboard-map", variant, std::to_string(code), std::to_string(type),
						   std::to_string(static_cast<uint32_t>(value)),
						   std::to_string(static_cast<uint32_t>(value >> 32)), std::to_string(incomingError)},
						  result))
		return 0;
	return result;
}

} // namespace

namespace user32 {

HKL WINAPI GetKeyboardLayout(DWORD idThread) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetKeyboardLayout(%u)\n", idThread);
	if (idThread && idThread != kernel32::GetCurrentThreadId()) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return 0;
	}
	const DWORD incomingError = kernel32::getLastError();
	HKL layout = 0;
	if (!readLayout(layout))
		return 0;
	kernel32::setLastError(incomingError);
	return layout;
}

BOOL WINAPI GetKeyboardLayoutNameW(LPWSTR name) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetKeyboardLayoutNameW(%p)\n", name);
	if (!name) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	const DWORD incomingError = kernel32::getLastError();
	HKL layout = 0;
	std::array<WCHAR, kLayoutNameUnits> currentName{};
	if (!readLayout(layout, &currentName))
		return FALSE;
	std::memcpy(name, currentName.data(), sizeof(currentName));
	kernel32::setLastError(incomingError);
	return TRUE;
}

SHORT WINAPI VkKeyScanExW(WCHAR character, HKL requested) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("VkKeyScanExW(0x%04x, 0x%llx)\n", character,
			  static_cast<unsigned long long>(static_cast<GUEST_PTR>(requested)));
	const DWORD incomingError = kernel32::getLastError();
	if (!wasIssuedLayout(requested)) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return -1;
	}
	const uint64_t value = static_cast<GUEST_PTR>(requested);
	uint32_t result = 0;
	if (!readScalarResult({"keyboard-vk-scan", std::to_string(character), std::to_string(static_cast<uint32_t>(value)),
						   std::to_string(static_cast<uint32_t>(value >> 32)), std::to_string(incomingError)},
						  result))
		return -1;
	if (result > UINT16_MAX) {
		kernel32::setLastError(ERROR_INVALID_DATA);
		return -1;
	}
	return static_cast<SHORT>(static_cast<WORD>(result));
}

UINT WINAPI MapVirtualKeyA(UINT code, UINT type) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("MapVirtualKeyA(%u, %u)\n", code, type);
	return mapVirtualKey(code, type, "a", 0);
}

UINT WINAPI MapVirtualKeyW(UINT code, UINT type) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("MapVirtualKeyW(%u, %u)\n", code, type);
	return mapVirtualKey(code, type, "w", 0);
}

UINT WINAPI MapVirtualKeyExA(UINT code, UINT type, HKL layout) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("MapVirtualKeyExA(%u, %u, 0x%llx)\n", code, type,
			  static_cast<unsigned long long>(static_cast<GUEST_PTR>(layout)));
	return mapVirtualKey(code, type, "ex-a", layout);
}

int WINAPI ToUnicode(UINT key, UINT scan, const BYTE *state, LPWSTR output, int capacity, UINT flags) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("ToUnicode(%u, %u, %p, %p, %d, %u)\n", key, scan, state, output, capacity, flags);
	if (!(flags & 4) || !state || !output || capacity <= 0 || capacity > kMaxUnicodeOutputUnits) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return 0;
	}
	const DWORD incomingError = kernel32::getLastError();
	HKL layout = 0;
	if (!readLayout(layout))
		return 0;
	const uint64_t layoutValue = static_cast<GUEST_PTR>(layout);
	const auto stateHex = wibo::provider::encodeBytes(std::string_view(reinterpret_cast<const char *>(state), 256));
	const size_t outputBytes = static_cast<size_t>(capacity) * sizeof(WCHAR);
	const auto outputHex =
		wibo::provider::encodeBytes(std::string_view(reinterpret_cast<const char *>(output), outputBytes));
	std::vector<uint8_t> response;
	if (!wibo::provider::request({"keyboard-to-unicode", std::to_string(key), std::to_string(scan),
								  std::to_string(flags), std::to_string(capacity),
								  std::to_string(static_cast<uint32_t>(layoutValue)),
								  std::to_string(static_cast<uint32_t>(layoutValue >> 32)), stateHex, outputHex,
								  std::to_string(incomingError)},
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
		kernel32::setLastError(reader.done() ? providerError(status) : ERROR_INVALID_DATA);
		return 0;
	}
	uint32_t rawResult = 0, nativeError = 0;
	std::vector<uint8_t> rawOutput;
	if (!reader.number(rawResult) || !reader.number(nativeError) || !reader.bytes(rawOutput) || !reader.done() ||
		rawOutput.size() != outputBytes || static_cast<int32_t>(rawResult) > capacity) {
		kernel32::setLastError(ERROR_INVALID_DATA);
		return 0;
	}
	std::memcpy(output, rawOutput.data(), outputBytes);
	kernel32::setLastError(nativeError);
	return static_cast<int32_t>(rawResult);
}

} // namespace user32
