#include "ole32.h"

#include "context.h"
#include "errors.h"
#include "heap.h"
#include "kernel32/internal.h"

namespace {

constexpr int kGuidTextUnits = 39;
constexpr HRESULT E_INVALIDARG = static_cast<HRESULT>(0x80070057);
constexpr HRESULT E_OUTOFMEMORY = static_cast<HRESULT>(0x8007000E);
constexpr HRESULT CO_E_CLASSSTRING = static_cast<HRESULT>(0x800401F3);
constexpr WCHAR kHexDigits[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

void appendHex(WCHAR *&cursor, uint32_t value, unsigned digits) {
	for (unsigned index = digits; index > 0; --index)
		*cursor++ = kHexDigits[(value >> ((index - 1) * 4)) & 0xf];
}

void formatGuid(const GUID &guid, WCHAR *output) {
	WCHAR *cursor = output;
	*cursor++ = '{';
	appendHex(cursor, guid.Data1, 8);
	*cursor++ = '-';
	appendHex(cursor, guid.Data2, 4);
	*cursor++ = '-';
	appendHex(cursor, guid.Data3, 4);
	*cursor++ = '-';
	appendHex(cursor, guid.Data4[0], 2);
	appendHex(cursor, guid.Data4[1], 2);
	*cursor++ = '-';
	for (unsigned index = 2; index < 8; ++index)
		appendHex(cursor, guid.Data4[index], 2);
	*cursor++ = '}';
	*cursor = 0;
}

HRESULT allocateGuidText(const GUID *guid, GUEST_PTR *output) {
	if (!guid || !output)
		return E_INVALIDARG;
	*output = GUEST_NULL;
	auto *buffer = static_cast<LPWSTR>(wibo::heap::guestMalloc(kGuidTextUnits * sizeof(WCHAR), false));
	if (!buffer)
		return E_OUTOFMEMORY;
	formatGuid(*guid, buffer);
	*output = toGuestPtr(buffer);
	return S_OK;
}

} // namespace

namespace ole32 {

int WINAPI StringFromGUID2(const GUID *guid, LPWSTR output, int capacity) {
	HOST_CONTEXT_GUARD();
	if (!guid || !output) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	if (capacity < kGuidTextUnits)
		return 0;
	formatGuid(*guid, output);
	return kGuidTextUnits;
}

HRESULT WINAPI StringFromCLSID(const GUID *guid, GUEST_PTR *output) {
	HOST_CONTEXT_GUARD();
	return allocateGuidText(guid, output);
}

HRESULT WINAPI StringFromIID(const GUID *guid, GUEST_PTR *output) {
	HOST_CONTEXT_GUARD();
	return allocateGuidText(guid, output);
}

HRESULT WINAPI IIDFromString(LPCWSTR text, GUID *guid) {
	HOST_CONTEXT_GUARD();
	const HRESULT result = CLSIDFromString(text, guid);
	return result == CO_E_CLASSSTRING ? E_INVALIDARG : result;
}

} // namespace ole32
