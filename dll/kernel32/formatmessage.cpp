#include "kernel32/winbase.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "heap.h"
#include "kernel32/internal.h"
#include "system_provider.h"

#include <algorithm>
#include <cstring>

namespace {
constexpr DWORD kAllocate = 0x100;
constexpr DWORD kIgnoreInserts = 0x200;
constexpr DWORD kFromSystem = 0x1000;
constexpr DWORD kArgumentArray = 0x2000;
constexpr DWORD kSupportedFlags = 0xFF | kAllocate | kIgnoreInserts | kFromSystem | kArgumentArray;

DWORD formatSystemMessage(DWORD flags, DWORD messageId, DWORD language, LPVOID buffer, DWORD size, LPCVOID arguments,
						  bool wide) {
	auto fail = [](DWORD error) {
		kernel32::setLastError(error);
		return DWORD(0);
	};
	if (!buffer)
		return fail(ERROR_INVALID_PARAMETER);
	if (!(flags & kFromSystem) || (flags & ~kSupportedFlags) || (arguments && !(flags & kIgnoreInserts)))
		return fail(ERROR_NOT_SUPPORTED);
	std::vector<uint8_t> response;
	if (!wibo::provider::request({"format-message", wide ? "w" : "a", std::to_string(flags & ~kAllocate),
								  std::to_string(messageId), std::to_string(language)},
								 response))
		return fail(ERROR_NOT_SUPPORTED);
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	if (!reader.header(status))
		return fail(13);
	if (status)
		return fail(reader.done() ? static_cast<DWORD>(status) : 13);
	std::vector<uint8_t> message;
	const size_t unit = wide ? sizeof(WCHAR) : 1;
	if (!reader.bytes(message) || !reader.done() || message.empty() || message.size() % unit ||
		message.size() > 128 * 1024)
		return fail(13);
	const size_t characters = message.size() / unit;
	if (flags & kAllocate) {
		const size_t allocation = std::max<size_t>(size, characters + 1);
		if (allocation > 1024 * 1024 / unit)
			return fail(ERROR_NOT_ENOUGH_MEMORY);
		void *allocated = wibo::heap::guestMalloc(allocation * unit, true);
		if (!allocated)
			return fail(ERROR_NOT_ENOUGH_MEMORY);
		std::memcpy(allocated, message.data(), message.size());
		*static_cast<GUEST_PTR *>(buffer) = toGuestPtr(allocated);
	} else {
		if (size <= characters)
			return fail(ERROR_INSUFFICIENT_BUFFER);
		std::memcpy(buffer, message.data(), message.size());
		std::memset(static_cast<uint8_t *>(buffer) + message.size(), 0, unit);
	}
	return static_cast<DWORD>(characters);
}
} // namespace

namespace kernel32 {
DWORD WINAPI FormatMessageA(DWORD flags, LPCVOID source, DWORD messageId, DWORD language, LPSTR buffer, DWORD size,
							LPCVOID arguments) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FormatMessageA(0x%x, %p, %u, %u, %p, %u, %p)\n", flags, source, messageId, language, buffer, size,
			  arguments);
	return formatSystemMessage(flags, messageId, language, buffer, size, arguments, false);
}
DWORD WINAPI FormatMessageW(DWORD flags, LPCVOID source, DWORD messageId, DWORD language, LPWSTR buffer, DWORD size,
							LPCVOID arguments) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FormatMessageW(0x%x, %p, %u, %u, %p, %u, %p)\n", flags, source, messageId, language, buffer, size,
			  arguments);
	return formatSystemMessage(flags, messageId, language, buffer, size, arguments, true);
}
} // namespace kernel32
