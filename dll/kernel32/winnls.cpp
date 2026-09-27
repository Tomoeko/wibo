#include "winnls.h"

#include "context.h"
#include "errors.h"
#include "heap.h"
#include "internal.h"
#include "kernel32.h"
#include "kernel32_trampolines.h"
#include "strutil.h"
#include "system_provider.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr DWORD kNormIgnoreCase = 0x00000001;
constexpr DWORD LCID_INSTALLED = 0x00000001;
constexpr DWORD LCID_SUPPORTED = 0x00000002;
constexpr DWORD LCID_ALTERNATE_SORTS = 0x00000004;
constexpr LCID kEnUsLcid = 0x0409;
constexpr LCID kInvariantLcid = 0x007f;
constexpr DWORD LOCALE_ALLOW_NEUTRAL_NAMES = 0x08000000;
constexpr DWORD kLocaleNoUserOverride = 0x80000000u;
constexpr DWORD kLocaleReturnNumber = 0x20000000u;
constexpr DWORD kLocaleFlagMask = 0xF0000000u;
constexpr DWORD kMapSortKey = 0x00000400;
constexpr DWORD kMapHash = 0x00040000;
constexpr DWORD kMapSortHandle = 0x20000000;
constexpr size_t kMaxLocaleNameUnits = 85;
constexpr size_t kMaxNlsRequest = 64 * 1024;

std::string encodeWideBytes(const uint16_t *text, size_t units) {
	std::string bytes(units * 2, '\0');
	for (size_t index = 0; index < units; ++index) {
		bytes[index * 2] = static_cast<char>(text[index] & 0xff);
		bytes[index * 2 + 1] = static_cast<char>(text[index] >> 8);
	}
	return wibo::provider::encodeBytes(bytes);
}

bool encodeNlsLocale(LPCWSTR name, std::string &encoded) {
	encoded = "-";
	if (!name) {
		return true;
	}
	const size_t units = wstrnlen(name, kMaxLocaleNameUnits);
	if (units == kMaxLocaleNameUnits) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return false;
	}
	encoded = encodeWideBytes(name, units);
	return true;
}

bool countNlsStringUnits(LPCWSTR text, int count, size_t &remainingHexBytes, size_t &units) {
	const size_t unitLimit = remainingHexBytes / 4;
	if (count < 0) {
		const size_t length = wstrnlen(text, unitLimit);
		if (length == unitLimit) {
			kernel32::setLastError(ERROR_NOT_ENOUGH_MEMORY);
			return false;
		}
		units = length + 1;
	} else {
		units = static_cast<size_t>(count);
		if (units > unitLimit) {
			kernel32::setLastError(ERROR_NOT_ENOUGH_MEMORY);
			return false;
		}
	}
	if (units * sizeof(uint16_t) > std::numeric_limits<uintptr_t>::max() - reinterpret_cast<uintptr_t>(text)) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return false;
	}
	remainingHexBytes -= units * 4;
	return true;
}

bool readNlsResponseHeader(wibo::provider::Reader &reader) {
	int32_t status = 0;
	if (!reader.header(status)) {
		kernel32::setLastError(ERROR_INVALID_DATA);
		return false;
	}
	if (status == ERROR_SUCCESS) {
		return true;
	}
	if (!reader.done()) {
		kernel32::setLastError(ERROR_INVALID_DATA);
		return false;
	}
	kernel32::setLastError(status == wibo::provider::kUnavailable ? ERROR_NOT_SUPPORTED : static_cast<DWORD>(status));
	return false;
}

int compareStrings(const std::string &a, const std::string &b, DWORD dwCmpFlags) {
	for (size_t i = 0;; ++i) {
		if (i == a.size()) {
			if (i == b.size()) {
				return 2; // CSTR_EQUAL
			}
			return 1; // CSTR_LESS_THAN
		}
		if (i == b.size()) {
			return 3; // CSTR_GREATER_THAN
		}
		unsigned char c = static_cast<unsigned char>(a[i]);
		unsigned char d = static_cast<unsigned char>(b[i]);
		if (dwCmpFlags & kNormIgnoreCase) {
			if (c >= 'a' && c <= 'z') {
				c = static_cast<unsigned char>(c - ('a' - 'A'));
			}
			if (d >= 'a' && d <= 'z') {
				d = static_cast<unsigned char>(d - ('a' - 'A'));
			}
		}
		if (c != d) {
			return (c < d) ? 1 : 3;
		}
	}
}

std::string localeInfoString(int LCType) {
	switch (LCType) {
	case 4100: // LOCALE_IDEFAULTANSICODEPAGE
		return "28591";
	case 4097: // LOCALE_SENGLANGUAGE
		return "Lang";
	case 4098: // LOCALE_SENGCOUNTRY
		return "Country";
	case 0x1: // LOCALE_ILANGUAGE
		return "0001";
	case 0x15: // LOCALE_SINTLSYMBOL
		return "Currency";
	case 0x14: // LOCALE_SCURRENCY
		return "sCurrency";
	case 0x16: // LOCALE_SMONDECIMALSEP
		return ".";
	case 0x17: // LOCALE_SMONTHOUSANDSEP
		return ",";
	case 0x18: // LOCALE_SMONGROUPING
		return ";";
	case 0x50: // LOCALE_SPOSITIVESIGN
		return "";
	case 0x51: // LOCALE_SNEGATIVESIGN
		return "-";
	case 0x1A: // LOCALE_IINTLCURRDIGITS
	case 0x19: // LOCALE_ICURRDIGITS
		return "2";
	default:
		DEBUG_LOG("STUB: GetLocaleInfo LCType 0x%x not implemented\n", LCType);
		return "";
	}
}

} // namespace

namespace kernel32 {

UINT WINAPI GetACP() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetACP() -> %u\n", 28591);
	return 28591; // Latin1 (ISO/IEC 8859-1)
}

LANGID WINAPI GetSystemDefaultLangID() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: GetSystemDefaultLangID()\n");
	return 0;
}

LANGID WINAPI GetUserDefaultUILanguage() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: GetUserDefaultUILanguage()\n");
	return 0;
}

int WINAPI GetUserDefaultLocaleName(LPWSTR lpLocaleName, int cchLocaleName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetUserDefaultLocaleName(%p, %d)\n", lpLocaleName, cchLocaleName);
	if (!lpLocaleName || cchLocaleName < 0) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}

	constexpr char16_t localeName[] = u"en-US";
	constexpr int requiredChars = static_cast<int>(sizeof(localeName) / sizeof(char16_t));
	if (cchLocaleName < requiredChars) {
		setLastError(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}

	std::memcpy(lpLocaleName, localeName, sizeof(localeName));
	return requiredChars;
}

LCID WINAPI LocaleNameToLCID(LPCWSTR lpName, DWORD dwFlags) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("LocaleNameToLCID(%p, 0x%x)\n", lpName, dwFlags);
	if (dwFlags & ~LOCALE_ALLOW_NEUTRAL_NAMES) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	if (!lpName) {
		return kEnUsLcid;
	}

	std::string localeName = wideStringToString(lpName);
	if (localeName.empty()) {
		return kInvariantLcid;
	}

	std::string normalized = stringToLower(localeName);
	if (normalized == "en-us" || normalized == "en_us" || normalized == "!x-sys-default-locale") {
		return kEnUsLcid;
	}

	setLastError(ERROR_INVALID_PARAMETER);
	return 0;
}

BOOL WINAPI GetCPInfo(UINT CodePage, LPCPINFO lpCPInfo) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetCPInfo(%u, %p)\n", CodePage, lpCPInfo);
	(void)CodePage;

	if (!lpCPInfo) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	lpCPInfo->MaxCharSize = 1;
	std::fill(lpCPInfo->DefaultChar, lpCPInfo->DefaultChar + MAX_DEFAULTCHAR, 0);
	lpCPInfo->DefaultChar[0] = '?';
	std::fill(lpCPInfo->LeadByte, lpCPInfo->LeadByte + MAX_LEADBYTES, 0);
	return TRUE;
}

BOOL WINAPI GetCPInfoExW(UINT CodePage, DWORD dwFlags, LPCPINFOEXW lpCPInfoEx) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetCPInfoExW(%u, 0x%x, %p)\n", CodePage, dwFlags, lpCPInfoEx);
	if (!lpCPInfoEx || dwFlags) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (CodePage == 3) {
		// The provider does not receive the guest thread's locale state.
		setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	// ANSI metadata must describe the code page exposed by the existing facade.
	const UINT resolvedCodePage = CodePage == 0 ? GetACP() : CodePage;
	std::vector<uint8_t> response;
	if (!wibo::provider::request({"cp-info-ex-w", std::to_string(resolvedCodePage), std::to_string(dwFlags)},
								 response)) {
		setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	uint32_t result = 0;
	if (!reader.header(status) || !reader.number(result) || result > 1) {
		setLastError(ERROR_INVALID_DATA);
		return FALSE;
	}
	if (!result) {
		if (!reader.done()) {
			setLastError(ERROR_INVALID_DATA);
			return FALSE;
		}
		// Some code pages can fail with ERROR_SUCCESS; retain the separate result.
		setLastError(status == wibo::provider::kUnavailable ? ERROR_NOT_SUPPORTED : static_cast<DWORD>(status));
		return FALSE;
	}
	std::vector<uint8_t> output;
	if (status != ERROR_SUCCESS || !reader.bytes(output) || output.size() != sizeof(CPINFOEXW) || !reader.done()) {
		setLastError(ERROR_INVALID_DATA);
		return FALSE;
	}
	CPINFOEXW information{};
	std::memcpy(&information, output.data(), sizeof(information));
	if (!information.MaxCharSize || information.CodePage <= 3 ||
		(resolvedCodePage > 3 && information.CodePage != resolvedCodePage) ||
		wstrnlen(information.CodePageName, MAX_PATH) == MAX_PATH) {
		setLastError(ERROR_INVALID_DATA);
		return FALSE;
	}
	std::memcpy(lpCPInfoEx, &information, sizeof(information));
	return TRUE;
}

int WINAPI CompareStringA(LCID Locale, DWORD dwCmpFlags, LPCSTR lpString1, int cchCount1, LPCSTR lpString2,
						  int cchCount2) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CompareStringA(%u, %u, %s, %d, %s, %d)\n", Locale, dwCmpFlags, lpString1 ? lpString1 : "(null)",
			  cchCount1, lpString2 ? lpString2 : "(null)", cchCount2);
	(void)Locale;
	if (!lpString1 || !lpString2) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}

	if (cchCount1 < 0) {
		cchCount1 = static_cast<int>(strlen(lpString1));
	}
	if (cchCount2 < 0) {
		cchCount2 = static_cast<int>(strlen(lpString2));
	}

	std::string str1(lpString1, lpString1 + cchCount1);
	std::string str2(lpString2, lpString2 + cchCount2);
	return compareStrings(str1, str2, dwCmpFlags);
}

int WINAPI CompareStringW(LCID Locale, DWORD dwCmpFlags, LPCWCH lpString1, int cchCount1, LPCWCH lpString2,
						  int cchCount2) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CompareStringW(%u, %u, %p, %d, %p, %d)\n", Locale, dwCmpFlags, lpString1, cchCount1, lpString2,
			  cchCount2);
	(void)Locale;
	if (!lpString1 || !lpString2) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}

	std::string str1 = wideStringToString(lpString1, cchCount1);
	std::string str2 = wideStringToString(lpString2, cchCount2);
	return compareStrings(str1, str2, dwCmpFlags);
}

int WINAPI CompareStringEx(LPCWSTR lpLocaleName, DWORD dwCmpFlags, LPCWCH lpString1, int cchCount1, LPCWCH lpString2,
						   int cchCount2, LPNLSVERSIONINFO lpVersionInformation, LPVOID lpReserved, LONG_PTR lParam) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CompareStringEx(%p, 0x%x, %p, %d, %p, %d, %p, %p, 0x%llx)\n", lpLocaleName, dwCmpFlags, lpString1,
			  cchCount1, lpString2, cchCount2, lpVersionInformation, lpReserved,
			  static_cast<unsigned long long>(lParam));
	if (lpVersionInformation || lpReserved || lParam) {
		// Sort tokens and version information are not supported by this transport.
		setLastError(ERROR_NOT_SUPPORTED);
		return 0;
	}
	if (!lpString1 || !lpString2) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	if (!wibo::provider::configured()) {
		setLastError(ERROR_NOT_SUPPORTED);
		return 0;
	}
	std::string locale;
	if (!encodeNlsLocale(lpLocaleName, locale)) {
		return 0;
	}
	// Both inputs share the request budget, including any terminating NUL units.
	size_t remainingHexBytes = kMaxNlsRequest - locale.size() - 128;
	size_t leftUnits = 0;
	size_t rightUnits = 0;
	if (!countNlsStringUnits(lpString1, cchCount1, remainingHexBytes, leftUnits) ||
		!countNlsStringUnits(lpString2, cchCount2, remainingHexBytes, rightUnits)) {
		return 0;
	}
	std::vector<uint8_t> response;
	if (!wibo::provider::request({"compare-string-ex", std::to_string(dwCmpFlags), locale, std::to_string(cchCount1),
								  encodeWideBytes(lpString1, leftUnits), std::to_string(cchCount2),
								  encodeWideBytes(lpString2, rightUnits)},
								 response)) {
		setLastError(ERROR_NOT_SUPPORTED);
		return 0;
	}
	wibo::provider::Reader reader(response);
	if (!readNlsResponseHeader(reader)) {
		return 0;
	}
	uint32_t result = 0;
	if (!reader.number(result) || result < 1 || result > 3 || !reader.done()) {
		setLastError(ERROR_INVALID_DATA);
		return 0;
	}
	return static_cast<int>(result);
}

BOOL WINAPI IsValidCodePage(UINT CodePage) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("IsValidCodePage(%u)\n", CodePage);
	(void)CodePage;
	return TRUE;
}

BOOL WINAPI IsValidLocale(LCID Locale, DWORD dwFlags) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("IsValidLocale(%u, 0x%x)\n", Locale, dwFlags);
	(void)Locale;
	if (dwFlags != 0 && (dwFlags & ~(LCID_INSTALLED | LCID_SUPPORTED | LCID_ALTERNATE_SORTS)) != 0) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	return TRUE;
}

int WINAPI GetLocaleInfoA(LCID Locale, LCTYPE LCType, LPSTR lpLCData, int cchData) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetLocaleInfoA(%u, %u, %p, %d)\n", Locale, LCType, lpLCData, cchData);
	(void)Locale;

	std::string value = localeInfoString(static_cast<int>(LCType));
	size_t required = value.size() + 1;

	if (cchData == 0) {
		return static_cast<int>(required);
	}
	if (!lpLCData || cchData < 0) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	if (static_cast<size_t>(cchData) < required) {
		setLastError(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}

	std::memcpy(lpLCData, value.c_str(), required);
	return static_cast<int>(required);
}

int WINAPI GetLocaleInfoW(LCID Locale, LCTYPE LCType, LPWSTR lpLCData, int cchData) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetLocaleInfoW(%u, %u, %p, %d)\n", Locale, LCType, lpLCData, cchData);
	(void)Locale;

	std::string info = localeInfoString(static_cast<int>(LCType));
	auto wide = stringToWideString(info.c_str());
	size_t required = wide.size();

	if (cchData == 0) {
		return static_cast<int>(required);
	}
	if (!lpLCData || cchData < 0) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	if (static_cast<size_t>(cchData) < required) {
		setLastError(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}

	std::memcpy(lpLCData, wide.data(), required * sizeof(uint16_t));
	return static_cast<int>(required);
}

int WINAPI GetLocaleInfoEx(LPCWSTR lpLocaleName, LCTYPE LCType, LPWSTR lpLCData, int cchData) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetLocaleInfoEx(%p, %u, %p, %d)\n", lpLocaleName, LCType, lpLCData, cchData);

	if (cchData < 0) {
		setLastError(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}

	DWORD lctypeFlags = static_cast<DWORD>(LCType) & kLocaleFlagMask;
	constexpr DWORD kSupportedFlags = kLocaleNoUserOverride;
	if ((lctypeFlags & kLocaleReturnNumber) != 0 || (lctypeFlags & ~kSupportedFlags) != 0) {
		setLastError(ERROR_INVALID_FLAGS);
		return 0;
	}

	if (lpLocaleName) {
		std::string localeName = wideStringToString(reinterpret_cast<const uint16_t *>(lpLocaleName));
		std::string normalized = stringToLower(localeName);
		if (!normalized.empty() && normalized != "en-us" && normalized != "en_us" &&
			normalized != "!x-sys-default-locale") {
			DEBUG_LOG("GetLocaleInfoEx: unsupported locale name '%s'\n", localeName.c_str());
			setLastError(ERROR_INVALID_PARAMETER);
			return 0;
		}
	}

	DWORD baseType = static_cast<DWORD>(LCType) & ~kLocaleFlagMask;
	std::string info = localeInfoString(static_cast<int>(baseType));
	auto wide = stringToWideString(info.c_str());
	size_t required = wide.size();

	if (cchData == 0) {
		return static_cast<int>(required);
	}
	if (!lpLCData) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	if (static_cast<size_t>(cchData) < required) {
		setLastError(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}

	std::memcpy(lpLCData, wide.data(), required * sizeof(uint16_t));
	return static_cast<int>(required);
}

BOOL WINAPI EnumSystemLocalesA(LOCALE_ENUMPROCA lpLocaleEnumProc, DWORD dwFlags) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("EnumSystemLocalesA(%p, 0x%x)\n", lpLocaleEnumProc, dwFlags);
	(void)dwFlags;
	if (!lpLocaleEnumProc) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	constexpr char defaultLocaleId[] = "00000409"; // en-US
	char *localeId = reinterpret_cast<char *>(wibo::heap::guestMalloc(sizeof(defaultLocaleId)));
	if (!localeId) {
		setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	std::memcpy(localeId, defaultLocaleId, sizeof(defaultLocaleId));
	BOOL ret = call_LOCALE_ENUMPROCA(lpLocaleEnumProc, localeId);
	wibo::heap::guestFree(localeId);
	return ret;
}

LCID WINAPI GetUserDefaultLCID() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: GetUserDefaultLCID()\n");
	return 0x0409; // en-US
}

BOOL WINAPI IsDBCSLeadByte(BYTE TestChar) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: IsDBCSLeadByte(%u)\n", TestChar);
	(void)TestChar;
	return FALSE;
}

BOOL WINAPI IsDBCSLeadByteEx(UINT CodePage, BYTE TestChar) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("IsDBCSLeadByteEx(%u, %u)\n", CodePage, TestChar);

	auto inRanges = [TestChar](std::initializer_list<std::pair<uint8_t, uint8_t>> ranges) -> BOOL {
		for (const auto &range : ranges) {
			if (TestChar >= range.first && TestChar <= range.second) {
				return TRUE;
			}
		}
		return FALSE;
	};

	setLastError(ERROR_SUCCESS);
	switch (CodePage) {
	case 932: // Shift-JIS
		return inRanges({{0x81, 0x9F}, {0xE0, 0xFC}});
	case 936:  // GBK
	case 949:  // Korean
	case 950:  // Big5
	case 1361: // Johab
		return inRanges({{0x81, 0xFE}});
	case 0: // CP_ACP
	case 1: // CP_OEMCP
	case 2: // CP_MACCP
	case 3: // CP_THREAD_ACP
	default:
		return FALSE;
	}
}

int WINAPI LCMapStringW(LCID Locale, DWORD dwMapFlags, LPCWCH lpSrcStr, int cchSrc, LPWSTR lpDestStr, int cchDest) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("LCMapStringW(%u, 0x%x, %p, %d, %p, %d)\n", Locale, dwMapFlags, lpSrcStr, cchSrc, lpDestStr, cchDest);
	(void)Locale;
	if (!lpSrcStr || cchSrc == 0) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}

	bool nullTerminated = cchSrc < 0;
	size_t srcLen = nullTerminated ? (wstrlen(lpSrcStr) + 1) : static_cast<size_t>(cchSrc);
	if (srcLen == 0) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}

	if (!lpDestStr || cchDest == 0) {
		return static_cast<int>(srcLen);
	}
	if (cchDest < static_cast<int>(srcLen)) {
		setLastError(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}

	if (dwMapFlags & (0x00000400u | 0x00000800u)) { // LCMAP_SORTKEY | LCMAP_BYTEREV
		DEBUG_LOG("LCMapStringW: unsupported mapping flags 0x%x\n", dwMapFlags);
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}

	unsigned int casingFlags = dwMapFlags & (0x00000200u | 0x00000100u); // UPPERCASE | LOWERCASE
	std::vector<uint16_t> buffer(srcLen, 0);
	for (size_t i = 0; i < srcLen; ++i) {
		uint16_t ch = lpSrcStr[i];
		if (casingFlags == 0x00000200u) {
			buffer[i] = wcharToUpper(ch);
		} else if (casingFlags == 0x00000100u) {
			buffer[i] = wcharToLower(ch);
		} else {
			buffer[i] = ch;
		}
	}

	std::memcpy(lpDestStr, buffer.data(), srcLen * sizeof(uint16_t));
	return static_cast<int>(srcLen);
}

int WINAPI LCMapStringEx(LPCWSTR lpLocaleName, DWORD dwMapFlags, LPCWSTR lpSrcStr, int cchSrc, LPWSTR lpDestStr,
						 int cchDest, LPNLSVERSIONINFO lpVersionInformation, LPVOID lpReserved, LONG_PTR sortHandle) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("LCMapStringEx(%p, 0x%x, %p, %d, %p, %d, %p, %p, 0x%llx)\n", lpLocaleName, dwMapFlags, lpSrcStr, cchSrc,
			  lpDestStr, cchDest, lpVersionInformation, lpReserved, static_cast<unsigned long long>(sortHandle));
	if ((dwMapFlags & (kMapSortHandle | kMapHash)) || lpVersionInformation || lpReserved || sortHandle) {
		// Hashing, sort tokens, and versioned requests are outside this provider operation.
		setLastError(ERROR_NOT_SUPPORTED);
		return 0;
	}
	if (!lpSrcStr || !cchSrc || cchDest < 0) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	if (!lpDestStr && cchDest) {
		setLastError(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}
	if (!wibo::provider::configured()) {
		setLastError(ERROR_NOT_SUPPORTED);
		return 0;
	}
	std::string locale;
	if (!encodeNlsLocale(lpLocaleName, locale)) {
		return 0;
	}
	// Reserve room for framing, the operation, and numeric arguments. Each UTF-16
	// source unit takes four characters when encoded as hexadecimal bytes.
	size_t remainingHexBytes = kMaxNlsRequest - locale.size() - 128;
	size_t sourceUnits = 0;
	if (!countNlsStringUnits(lpSrcStr, cchSrc, remainingHexBytes, sourceUnits)) {
		return 0;
	}
	const size_t outputUnitSize = dwMapFlags & kMapSortKey ? 1 : sizeof(uint16_t);
	if (static_cast<size_t>(cchDest) > wibo::provider::kMaxResponse / outputUnitSize) {
		setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return 0;
	}
	const size_t sourceBytes = sourceUnits * sizeof(uint16_t);
	const size_t destinationBytes = static_cast<size_t>(cchDest) * outputUnitSize;
	const uintptr_t sourceAddress = reinterpret_cast<uintptr_t>(lpSrcStr);
	const uintptr_t destinationAddress = reinterpret_cast<uintptr_t>(lpDestStr);
	if (destinationBytes > std::numeric_limits<uintptr_t>::max() - destinationAddress) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	if (destinationBytes && lpSrcStr != lpDestStr && sourceAddress < destinationAddress + destinationBytes &&
		destinationAddress < sourceAddress + sourceBytes) {
		// Overlapping subranges need buffer identity beyond the supported alias mode.
		setLastError(ERROR_NOT_SUPPORTED);
		return 0;
	}
	const char *destinationMode = !cchDest ? "0" : lpSrcStr == lpDestStr ? "2" : "1";
	std::vector<uint8_t> response;
	if (!wibo::provider::request({"lc-map-string-ex", std::to_string(dwMapFlags), locale, std::to_string(cchSrc),
								  encodeWideBytes(lpSrcStr, sourceUnits), std::to_string(cchDest), destinationMode},
								 response)) {
		setLastError(ERROR_NOT_SUPPORTED);
		return 0;
	}
	wibo::provider::Reader reader(response);
	if (!readNlsResponseHeader(reader)) {
		return 0;
	}
	uint32_t result = 0;
	std::vector<uint8_t> output;
	if (!reader.number(result) || !result || result > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
		!reader.bytes(output) || !reader.done() ||
		(cchDest == 0 ? !output.empty()
					  : result > static_cast<uint32_t>(cchDest) ||
							output.size() != static_cast<size_t>(result) * outputUnitSize)) {
		setLastError(ERROR_INVALID_DATA);
		return 0;
	}
	if (cchDest) {
		std::memcpy(lpDestStr, output.data(), output.size());
	}
	return static_cast<int>(result);
}

int WINAPI LCMapStringA(LCID Locale, DWORD dwMapFlags, LPCCH lpSrcStr, int cchSrc, LPSTR lpDestStr, int cchDest) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("LCMapStringA(%u, 0x%x, %p, %d, %p, %d)\n", Locale, dwMapFlags, lpSrcStr, cchSrc, lpDestStr, cchDest);
	if (!lpSrcStr) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	int length = cchSrc;
	if (length < 0) {
		length = static_cast<int>(strlen(lpSrcStr)) + 1;
	}

	auto wideSrc = stringToWideString(lpSrcStr, static_cast<size_t>(length));
	std::vector<uint16_t> wideDest(std::max(cchDest, 0));
	int wideResult =
		LCMapStringW(Locale, dwMapFlags, wideSrc.data(), length, wideDest.empty() ? nullptr : wideDest.data(), cchDest);
	if (wideResult == 0) {
		setLastError(ERROR_SUCCESS);
		return 0;
	}

	if (!lpDestStr || cchDest == 0) {
		return wideResult;
	}

	auto mapped = wideStringToString(wideDest.data(), wideResult);
	size_t bytesToCopy = mapped.size() + 1;
	if (static_cast<size_t>(cchDest) < bytesToCopy) {
		setLastError(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}
	std::memcpy(lpDestStr, mapped.c_str(), bytesToCopy);
	return wideResult;
}

} // namespace kernel32
