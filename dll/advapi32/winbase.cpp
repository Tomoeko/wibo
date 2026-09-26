#include "winbase.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "internal.h"
#include "kernel32/internal.h"
#include "strutil.h"
#include "system_provider.h"

#include <algorithm>
#include <cstring>
#include <mutex>

namespace {

constexpr WCHAR kAccountSystem[] = {u'S', u'Y', u'S', u'T', u'E', u'M', u'\0'};
constexpr WCHAR kDomainNtAuthority[] = {u'N', u'T', u' ', u'A', u'U', u'T', u'H', u'O', u'R', u'I', u'T', u'Y', u'\0'};
constexpr BYTE kNtAuthority[6] = {0, 0, 0, 0, 0, 5};

std::mutex g_privilegeMapMutex;
std::unordered_map<std::string, LUID> g_privilegeLuidCache;

bool isLocalSystemSid(const Sid *sid) {
	if (!sid) {
		return false;
	}
	if (sid->Revision != 1 || sid->SubAuthorityCount != 1) {
		return false;
	}
	for (size_t i = 0; i < std::size(kNtAuthority); ++i) {
		if (sid->IdentifierAuthority.Value[i] != kNtAuthority[i]) {
			return false;
		}
	}
	return sid->SubAuthority[0] == SECURITY_LOCAL_SYSTEM_RID;
}

std::string normalizePrivilegeName(const std::string &name) {
	std::string normalized;
	normalized.reserve(name.size());
	for (unsigned char ch : name) {
		normalized.push_back(static_cast<char>(std::tolower(ch)));
	}
	return normalized;
}

LUID generateDeterministicLuid(const std::string &normalizedName) {
	uint32_t hash = 2166136261u;
	for (unsigned char ch : normalizedName) {
		hash ^= ch;
		hash *= 16777619u;
	}
	if (hash == 0) {
		hash = 1;
	}
	LUID luid{};
	luid.LowPart = hash;
	luid.HighPart = 0;
	return luid;
}

LUID lookupOrGeneratePrivilegeLuid(const std::string &normalizedName) {
	std::lock_guard<std::mutex> lock(g_privilegeMapMutex);
	static const std::unordered_map<std::string, uint32_t> predefined = {
		{"se_debug_name", 0x14},
		{"se_shutdown_name", 0x13},
	};
	auto it = g_privilegeLuidCache.find(normalizedName);
	if (it != g_privilegeLuidCache.end()) {
		return it->second;
	}
	LUID luid{};
	auto predefinedIt = predefined.find(normalizedName);
	if (predefinedIt != predefined.end()) {
		luid.LowPart = predefinedIt->second;
		luid.HighPart = 0;
	} else {
		luid = generateDeterministicLuid(normalizedName);
	}
	g_privilegeLuidCache[normalizedName] = luid;
	return luid;
}

std::string encodeBytes(LPCSTR text) {
	std::string result;
	constexpr char digits[] = "0123456789abcdef";
	if (text)
		for (; *text; ++text) {
			const auto byte = static_cast<unsigned char>(*text);
			result.push_back(digits[byte >> 4]);
			result.push_back(digits[byte & 15]);
		}
	return result;
}

BOOL providerFailure(DWORD status) {
	kernel32::setLastError(status);
	return FALSE;
}

bool validText(std::u16string_view text) { return text.find(u'\0') == std::u16string_view::npos; }

DWORD readProviderUser(std::u16string &wide, std::vector<uint8_t> &narrow) {
	std::vector<uint8_t> response;
	if (!wibo::provider::request({"user-name"}, response))
		return ERROR_NOT_SUPPORTED;
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	if (!reader.header(status))
		return 13;
	if (status == ERROR_SUCCESS && (!reader.text(wide) || !reader.bytes(narrow) || !validText(wide) ||
									std::find(narrow.begin(), narrow.end(), 0) != narrow.end()))
		return 13;
	return reader.done() ? status : 13;
}

BOOL lookupAccount(const std::vector<std::string> &arguments, bool ansi, PSID sid, LPDWORD sidSize, void *domain,
				   LPDWORD domainSize, SID_NAME_USE *use) {
	if (!sidSize || !domainSize || !use || (!sid && *sidSize) || (!domain && *domainSize))
		return providerFailure(ERROR_INVALID_PARAMETER);
	std::vector<uint8_t> response;
	if (!wibo::provider::request(arguments, response))
		return providerFailure(ERROR_NOT_SUPPORTED);
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	if (!reader.header(status))
		return providerFailure(13);
	if (status != ERROR_SUCCESS)
		return providerFailure(reader.done() ? status : 13);
	std::vector<uint8_t> identifier, domainBytes;
	uint32_t kind = 0;
	if (!reader.bytes(identifier) || !reader.bytes(domainBytes) || !reader.number(kind) || !reader.done() ||
		identifier.size() < 8 || identifier[0] != 1 || identifier[1] > 15 ||
		identifier.size() != 8 + 4 * size_t(identifier[1]) || kind < 1 || kind > 11 ||
		(!ansi && domainBytes.size() % 2))
		return providerFailure(13);
	const size_t unit = ansi ? 1 : 2;
	for (size_t i = 0; i < domainBytes.size(); i += unit) {
		if (domainBytes[i] == 0 && (ansi || domainBytes[i + 1] == 0))
			return providerFailure(13);
	}
	const DWORD requiredDomain = static_cast<DWORD>(domainBytes.size() / unit);
	if (!sid || *sidSize < identifier.size() || !domain || *domainSize <= requiredDomain) {
		*sidSize = static_cast<DWORD>(identifier.size());
		*domainSize = requiredDomain + 1;
		return providerFailure(ERROR_INSUFFICIENT_BUFFER);
	}
	std::memcpy(sid, identifier.data(), identifier.size());
	std::memcpy(domain, domainBytes.data(), domainBytes.size());
	std::memset(static_cast<uint8_t *>(domain) + domainBytes.size(), 0, unit);
	*sidSize = static_cast<DWORD>(identifier.size());
	*domainSize = requiredDomain;
	*use = static_cast<SID_NAME_USE>(kind);
	return TRUE;
}

} // namespace

namespace advapi32 {

BOOL WINAPI LookupAccountNameA(LPCSTR system, LPCSTR account, PSID sid, LPDWORD sidSize, LPSTR domain,
							   LPDWORD domainSize, SID_NAME_USE *use) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("LookupAccountNameA(%p, %p, %p, %p, %p, %p, %p)\n", system, account, sid, sidSize, domain, domainSize,
			  use);
	if (!account)
		return providerFailure(ERROR_INVALID_PARAMETER);
	return lookupAccount({"account-lookup-a", encodeBytes(system), encodeBytes(account)}, true, sid, sidSize, domain,
						 domainSize, use);
}

BOOL WINAPI LookupAccountNameW(LPCWSTR system, LPCWSTR account, PSID sid, LPDWORD sidSize, LPWSTR domain,
							   LPDWORD domainSize, SID_NAME_USE *use) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("LookupAccountNameW(%p, %p, %p, %p, %p, %p, %p)\n", system, account, sid, sidSize, domain, domainSize,
			  use);
	if (!account)
		return providerFailure(ERROR_INVALID_PARAMETER);
	std::string systemText, accountText;
	const auto view = [](LPCWSTR text) {
		return std::u16string_view(reinterpret_cast<const char16_t *>(text), text ? wstrlen(text) : 0);
	};
	if (!wibo::provider::encodeUtf8(view(system), systemText) ||
		!wibo::provider::encodeUtf8(view(account), accountText))
		return providerFailure(ERROR_INVALID_PARAMETER);
	return lookupAccount({"account-lookup-w", systemText, accountText}, false, sid, sidSize, domain, domainSize, use);
}

BOOL WINAPI LookupAccountSidW(LPCWSTR lpSystemName, PSID Sid, LPWSTR Name, LPDWORD cchName,
								LPWSTR ReferencedDomainName, LPDWORD cchReferencedDomainName, SID_NAME_USE *peUse) {
	HOST_CONTEXT_GUARD();
	std::string systemName = lpSystemName ? wideStringToString(lpSystemName) : std::string("(null)");
	DEBUG_LOG("LookupAccountSidW(%s, %p, %p, %p, %p, %p, %p)\n", systemName.c_str(), Sid, Name, cchName,
			  ReferencedDomainName, cchReferencedDomainName, peUse);
	if (!Sid || !cchName || !cchReferencedDomainName || !peUse) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	auto *sidStruct = reinterpret_cast<const struct Sid *>(Sid);
	if (!isLocalSystemSid(sidStruct)) {
		kernel32::setLastError(ERROR_NONE_MAPPED);
		return FALSE;
	}
	DWORD requiredAccount = static_cast<DWORD>(wstrlen(kAccountSystem));
	DWORD requiredDomain = static_cast<DWORD>(wstrlen(kDomainNtAuthority));
	if (!Name || *cchName <= requiredAccount || !ReferencedDomainName || *cchReferencedDomainName <= requiredDomain) {
		*cchName = requiredAccount + 1;
		*cchReferencedDomainName = requiredDomain + 1;
		kernel32::setLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	std::copy_n(kAccountSystem, requiredAccount + 1, Name);
	std::copy_n(kDomainNtAuthority, requiredDomain + 1, ReferencedDomainName);
	*peUse = SidTypeWellKnownGroup;
	*cchName = requiredAccount;
	*cchReferencedDomainName = requiredDomain;
	return TRUE;
}

BOOL WINAPI LookupPrivilegeValueA(LPCSTR lpSystemName, LPCSTR lpName, PLUID lpLuid) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("LookupPrivilegeValueA(%s, %s, %p)\n", lpSystemName ? lpSystemName : "(null)", lpName ? lpName : "(null)",
			  lpLuid);
	(void)lpSystemName; // only local lookup supported
	if (!lpName || !lpLuid) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	std::string normalized = normalizePrivilegeName(lpName);
	LUID luid = lookupOrGeneratePrivilegeLuid(normalized);
	*lpLuid = luid;
	return TRUE;
}

BOOL WINAPI LookupPrivilegeValueW(LPCWSTR lpSystemName, LPCWSTR lpName, PLUID lpLuid) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("LookupPrivilegeValueW(%p, %p, %p)\n", lpSystemName, lpName, lpLuid);
	(void)lpSystemName; // only local lookup supported
	if (!lpName || !lpLuid) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	std::string ansiName = wideStringToString(lpName);
	std::string normalized = normalizePrivilegeName(ansiName);
	LUID luid = lookupOrGeneratePrivilegeLuid(normalized);
	*lpLuid = luid;
	return TRUE;
}

BOOL WINAPI GetUserNameA(LPSTR lpBuffer, LPDWORD pcbBuffer) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetUserNameA(%p, %p)\n", lpBuffer, pcbBuffer);
	if (!pcbBuffer) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (wibo::provider::configured()) {
		std::u16string wide;
		std::vector<uint8_t> narrow;
		const DWORD status = readProviderUser(wide, narrow);
		if (status != ERROR_SUCCESS)
			return providerFailure(status);
		const DWORD needed = static_cast<DWORD>(narrow.size() + 1);
		if (!lpBuffer || *pcbBuffer < needed) {
			*pcbBuffer = needed;
			return providerFailure(ERROR_INSUFFICIENT_BUFFER);
		}
		std::memcpy(lpBuffer, narrow.data(), narrow.size());
		lpBuffer[narrow.size()] = 0;
		*pcbBuffer = needed;
		return TRUE;
	}
	const char *name = "SYSTEM";
	size_t needed = std::strlen(name) + 1;
	if (!lpBuffer || *pcbBuffer < needed) {
		*pcbBuffer = static_cast<DWORD>(needed);
		kernel32::setLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	std::memcpy(lpBuffer, name, needed);
	*pcbBuffer = static_cast<DWORD>(needed);
	return TRUE;
}

BOOL WINAPI GetUserNameW(LPWSTR lpBuffer, LPDWORD pcbBuffer) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetUserNameW(%p, %p)\n", lpBuffer, pcbBuffer);
	if (!pcbBuffer) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (wibo::provider::configured()) {
		std::u16string wide;
		std::vector<uint8_t> narrow;
		const DWORD status = readProviderUser(wide, narrow);
		if (status != ERROR_SUCCESS)
			return providerFailure(status);
		const DWORD needed = static_cast<DWORD>(wide.size() + 1);
		if (!lpBuffer || *pcbBuffer < needed) {
			*pcbBuffer = needed;
			return providerFailure(ERROR_INSUFFICIENT_BUFFER);
		}
		std::memcpy(lpBuffer, wide.c_str(), needed * sizeof(WCHAR));
		*pcbBuffer = needed;
		return TRUE;
	}
	size_t needed = wstrlen(kAccountSystem) + 1;
	if (!lpBuffer || *pcbBuffer < needed) {
		*pcbBuffer = static_cast<DWORD>(needed);
		kernel32::setLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	std::memcpy(lpBuffer, kAccountSystem, needed * sizeof(WCHAR));
	*pcbBuffer = static_cast<DWORD>(needed);
	return TRUE;
}

} // namespace advapi32
