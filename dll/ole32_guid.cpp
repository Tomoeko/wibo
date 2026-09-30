#include "ole32.h"

#include "advapi32/winreg.h"
#include "context.h"
#include "errors.h"
#include "heap.h"
#include "kernel32/internal.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int kGuidTextUnits = 39;
constexpr HRESULT E_INVALIDARG = static_cast<HRESULT>(0x80070057);
constexpr HRESULT E_OUTOFMEMORY = static_cast<HRESULT>(0x8007000E);
constexpr HRESULT CO_E_CLASSSTRING = static_cast<HRESULT>(0x800401F3);
constexpr HRESULT REGDB_E_CLASSNOTREG = static_cast<HRESULT>(0x80040154);
constexpr HRESULT REGDB_E_READREGDB = static_cast<HRESULT>(0x80040150);
constexpr WCHAR kHexDigits[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
constexpr DWORD kRegistryString = 0x2; // RRF_RT_REG_SZ

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

LSTATUS readClassString(const std::u16string &subkey, std::u16string &value) {
	const auto *path = reinterpret_cast<LPCWSTR>(subkey.c_str());
	DWORD size = 0;
	LSTATUS status = advapi32::RegGetValueW(HKEY_CLASSES_ROOT, path, nullptr, kRegistryString, nullptr, nullptr, &size);
	if (status != ERROR_SUCCESS)
		return status;
	if (size < sizeof(WCHAR) || size % sizeof(WCHAR))
		return ERROR_INVALID_DATA;
	std::vector<WCHAR> buffer(size / sizeof(WCHAR) + 1);
	status = advapi32::RegGetValueW(HKEY_CLASSES_ROOT, path, nullptr, kRegistryString, nullptr, buffer.data(), &size);
	if (status != ERROR_SUCCESS)
		return status;
	if (size < sizeof(WCHAR) || size % sizeof(WCHAR) || buffer[size / sizeof(WCHAR) - 1] != 0)
		return ERROR_INVALID_DATA;
	value.assign(reinterpret_cast<const char16_t *>(buffer.data()));
	return ERROR_SUCCESS;
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

HRESULT WINAPI CLSIDFromProgID(LPCWSTR progId, GUID *guid) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CLSIDFromProgID(%p, %p)\n", progId, guid);
	if (!guid)
		return E_INVALIDARG;
	if (!progId || !*progId)
		return CO_E_CLASSSTRING;
	std::u16string subkey(reinterpret_cast<const char16_t *>(progId));
	subkey += u"\\CLSID";
	std::u16string text;
	const LSTATUS status = readClassString(subkey, text);
	if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND)
		return CO_E_CLASSSTRING;
	if (status != ERROR_SUCCESS)
		return REGDB_E_READREGDB;
	return CLSIDFromString(reinterpret_cast<LPCWSTR>(text.c_str()), guid);
}

HRESULT WINAPI ProgIDFromCLSID(const GUID *guid, GUEST_PTR *output) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("ProgIDFromCLSID(%p, %p)\n", guid, output);
	if (!guid || !output)
		return E_INVALIDARG;
	*output = GUEST_NULL;
	WCHAR guidText[kGuidTextUnits];
	formatGuid(*guid, guidText);
	std::u16string subkey = u"CLSID\\";
	subkey += reinterpret_cast<const char16_t *>(guidText);
	subkey += u"\\ProgID";
	std::u16string progId;
	const LSTATUS status = readClassString(subkey, progId);
	if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND)
		return REGDB_E_CLASSNOTREG;
	if (status != ERROR_SUCCESS)
		return REGDB_E_READREGDB;
	const size_t bytes = (progId.size() + 1) * sizeof(WCHAR);
	auto *buffer = static_cast<WCHAR *>(wibo::heap::guestMalloc(bytes, false));
	if (!buffer)
		return E_OUTOFMEMORY;
	std::memcpy(buffer, progId.c_str(), bytes);
	*output = toGuestPtr(buffer);
	return S_OK;
}

} // namespace ole32
