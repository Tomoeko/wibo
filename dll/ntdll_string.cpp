#include "ntdll.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "heap.h"

#include <cstddef>
#include <cstring>
#include <limits>

namespace {

constexpr std::size_t kMaximumCountedBytes = std::numeric_limits<USHORT>::max();
constexpr std::size_t kMaximumAnsiCharacters = kMaximumCountedBytes - 1;
constexpr std::size_t kMaximumUnicodeCharacters = kMaximumCountedBytes / sizeof(WCHAR) - 1;
constexpr NTSTATUS kNameTooLong = static_cast<NTSTATUS>(0xc0000106u);

static_assert(sizeof(WCHAR) == 2);
#ifdef WIBO_GUEST_64
static_assert(sizeof(STRING) == 16 && offsetof(STRING, Buffer) == 8);
static_assert(sizeof(UNICODE_STRING) == 16 && offsetof(UNICODE_STRING, Buffer) == 8);
#else
static_assert(sizeof(STRING) == 8 && offsetof(STRING, Buffer) == 4);
static_assert(sizeof(UNICODE_STRING) == 8 && offsetof(UNICODE_STRING, Buffer) == 4);
#endif

template <typename Descriptor, typename Character>
NTSTATUS initializeCountedString(Descriptor *destination, const Character *source, std::size_t maximumCharacters,
								 bool rejectTooLong) {
	std::size_t characters = 0;
	if (source) {
		// Inspect only through the first length that cannot fit in the descriptor.
		while (characters <= maximumCharacters && source[characters] != 0)
			++characters;
	}
	if (characters > maximumCharacters) {
		if (rejectTooLong)
			return kNameTooLong;
		characters = maximumCharacters;
	}

	// The descriptor borrows the source buffer. Only its named fields are written.
	destination->Length = static_cast<USHORT>(characters * sizeof(Character));
	destination->MaximumLength = static_cast<USHORT>((characters + (source ? 1 : 0)) * sizeof(Character));
	destination->Buffer = toGuestPtr(source);
	return STATUS_SUCCESS;
}

} // namespace

VOID WINAPI ntdll::RtlInitString(STRING *destination, LPCSTR source) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlInitString(%p, %p)\n", destination, source);
	initializeCountedString(destination, source, kMaximumAnsiCharacters, false);
}

VOID WINAPI ntdll::RtlInitAnsiString(ANSI_STRING *destination, LPCSTR source) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlInitAnsiString(%p, %p)\n", destination, source);
	initializeCountedString(destination, source, kMaximumAnsiCharacters, false);
}

VOID WINAPI ntdll::RtlInitUnicodeString(UNICODE_STRING *destination, LPCWSTR source) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlInitUnicodeString(%p, %p)\n", destination, source);
	initializeCountedString(destination, source, kMaximumUnicodeCharacters, false);
}

BOOLEAN WINAPI ntdll::RtlCreateUnicodeString(UNICODE_STRING *destination, LPCWSTR source) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlCreateUnicodeString(%p, %p)\n", destination, source);
	if (!destination || !source)
		return FALSE;
	std::size_t characters = 0;
	while (characters <= kMaximumUnicodeCharacters && source[characters] != 0)
		++characters;
	if (characters > kMaximumUnicodeCharacters)
		return FALSE;
	const std::size_t bytes = (characters + 1) * sizeof(WCHAR);
	void *buffer = wibo::heap::guestMalloc(bytes);
	if (!buffer)
		return FALSE;
	std::memcpy(buffer, source, bytes);
	destination->Length = static_cast<USHORT>(characters * sizeof(WCHAR));
	destination->MaximumLength = static_cast<USHORT>(bytes);
	destination->Buffer = toGuestPtr(buffer);
	return TRUE;
}

VOID WINAPI ntdll::RtlFreeUnicodeString(UNICODE_STRING *destination) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlFreeUnicodeString(%p)\n", destination);
	if (destination && destination->Buffer) {
		wibo::heap::guestFree(fromGuestPtr<void>(destination->Buffer));
		*destination = {};
	}
}

NTSTATUS WINAPI ntdll::RtlInitAnsiStringEx(ANSI_STRING *destination, LPCSTR source) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlInitAnsiStringEx(%p, %p)\n", destination, source);
	return initializeCountedString(destination, source, kMaximumAnsiCharacters, true);
}

NTSTATUS WINAPI ntdll::RtlInitUnicodeStringEx(UNICODE_STRING *destination, LPCWSTR source) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlInitUnicodeStringEx(%p, %p)\n", destination, source);
	return initializeCountedString(destination, source, kMaximumUnicodeCharacters, true);
}
