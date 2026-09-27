#include "processenv.h"

#include "context.h"
#include "errors.h"
#include "files.h"
#include "heap.h"
#include "internal.h"
#include "strutil.h"
#include "system_provider.h"
#include "types.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mimalloc.h>
#include <optional>
#include <string>
#include <string_view>
#include <strings.h>
#include <unistd.h>
#include <utility>
#include <vector>

#ifdef __APPLE__
extern char **environ;
#endif

namespace {

GUEST_PTR g_commandLineA = GUEST_NULL;
GUEST_PTR g_commandLineW = GUEST_NULL;
constexpr DWORD kNoUnicodeTranslation = 1113;

std::string convertEnvValueForWindows(const std::string &name, const char *rawValue) {
	if (!rawValue) {
		return {};
	}
	if (strcasecmp(name.c_str(), "PATH") != 0) {
		if (strcasecmp(name.c_str(), "TMP") != 0 && strcasecmp(name.c_str(), "TEMP") != 0) {
			return rawValue;
		}
		std::string path = rawValue;
		bool looksWindows =
			path.find('\\') != std::string::npos || (path.size() >= 2 && path[1] == ':' && path[0] != '/');
		if (looksWindows) {
			std::replace(path.begin(), path.end(), '/', '\\');
			return path;
		}
		return files::pathToWindows(std::filesystem::path(path));
	}
	std::string converted = files::hostPathListToWindows(rawValue);
	return converted.empty() ? std::string(rawValue) : converted;
}

const char *getenvCaseInsensitive(const std::string &name) {
	if (const char *exact = getenv(name.c_str())) {
		return exact;
	}
	for (char **work = environ; *work; ++work) {
		std::string_view entry(*work);
		size_t eq = entry.find('=');
		if (eq != std::string_view::npos && entry.size() >= eq + 1 && entry.compare(0, eq, name) == 0) {
			return entry.data() + eq + 1;
		}
		if (eq != std::string_view::npos && entry.size() >= eq + 1) {
			std::string envName(entry.substr(0, eq));
			if (strcasecmp(envName.c_str(), name.c_str()) == 0) {
				return entry.data() + eq + 1;
			}
		}
	}
	return nullptr;
}

void importEnvironmentDefaults() {
	if (!wibo::provider::configured())
		return;
	std::vector<uint8_t> response;
	if (!wibo::provider::request({"environment-defaults"}, response))
		return;
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	uint32_t count = 0;
	if (!reader.header(status) || status || !reader.number(count) || count > 64)
		return;
	std::vector<std::pair<std::string, std::string>> values;
	for (uint32_t i = 0; i < count; ++i) {
		std::vector<uint8_t> name, value;
		if (!reader.bytes(name) || name.empty() || name.size() > 256 || !reader.bytes(value) || value.size() > 32768 ||
			std::find(name.begin(), name.end(), 0) != name.end() ||
			std::find(name.begin(), name.end(), '=') != name.end() ||
			std::find(value.begin(), value.end(), 0) != value.end())
			return;
		values.emplace_back(std::string(name.begin(), name.end()), std::string(value.begin(), value.end()));
	}
	if (!reader.done())
		return;
	for (const auto &[name, value] : values)
		if (!getenvCaseInsensitive(name))
			setenv(name.c_str(), value.c_str(), 0);
}

void ensureTempEnvVariables() {
	static const bool initialized = [] {
		if (getenv("WIBO_ENVIRONMENT_INITIALIZED"))
			return true;
		importEnvironmentDefaults();
		const char *hostTemp = getenv("TMPDIR");
		if (!hostTemp || !*hostTemp) {
			hostTemp = "/tmp";
		}
		if (!getenvCaseInsensitive("TMP")) {
			setenv("TMP", hostTemp, 0);
		}
		if (!getenvCaseInsensitive("TEMP")) {
			setenv("TEMP", hostTemp, 0);
		}
		setenv("WIBO_ENVIRONMENT_INITIALIZED", "1", 1);
		return true;
	}();
	(void)initialized;
}

std::optional<std::string> getEnvValueForWindows(const std::string &name) {
	ensureTempEnvVariables();
	if (const char *rawValue = getenvCaseInsensitive(name)) {
		return convertEnvValueForWindows(name, rawValue);
	}
	return std::nullopt;
}

std::optional<std::string> expansionVariableName(std::string_view name) {
	return name.empty() ? std::nullopt : std::make_optional(std::string(name));
}

std::optional<std::string> expansionVariableName(std::u16string_view name) {
	if (name.empty()) {
		return std::nullopt;
	}
	// Environment storage uses wibo's current single-byte ACP. A wide name
	// outside that mapping must remain unknown, never alias its low bytes.
	std::string result;
	result.reserve(name.size());
	for (char16_t character : name) {
		if (character > 0xff) {
			return std::nullopt;
		}
		result.push_back(static_cast<char>(character));
	}
	return result;
}

template <typename Character, typename Append>
bool visitEnvironmentExpansion(std::basic_string_view<Character> source, Append append) {
	using View = std::basic_string_view<Character>;
	constexpr Character percent = static_cast<Character>('%');
	size_t cursor = 0;
	while (cursor < source.size()) {
		size_t opening = source.find(percent, cursor);
		if (opening == View::npos) {
			return append(source.substr(cursor), false);
		}
		if (!append(source.substr(cursor, opening - cursor), false)) {
			return false;
		}
		size_t closing = source.find(percent, opening + 1);
		if (closing == View::npos) {
			return append(source.substr(opening), false);
		}
		auto name = expansionVariableName(source.substr(opening + 1, closing - opening - 1));
		auto value = name ? getEnvValueForWindows(*name) : std::nullopt;
		if (value) {
			std::basic_string<Character> replacement;
			replacement.reserve(value->size());
			for (unsigned char character : *value) {
				replacement.push_back(static_cast<Character>(character));
			}
			if (!append(View(replacement), true)) {
				return false;
			}
		} else if (!append(source.substr(opening, closing - opening + 1), false)) {
			return false;
		}
		// Continue in the original source: percent expressions in values are
		// not recursively expanded, and unknown/empty names stay unchanged.
		cursor = closing + 1;
	}
	return true;
}

std::vector<std::string> prepareEnvStrings(size_t &totalSize) {
	ensureTempEnvVariables();
	std::vector<std::string> strings;
	totalSize = 0;
	for (char **work = environ; *work; ++work) {
		std::string s = *work;
		size_t eq = s.find('=');
		if (eq != std::string::npos) {
			std::string name = s.substr(0, eq);
			std::string value = s.substr(eq + 1);
			std::string converted = convertEnvValueForWindows(name, value.c_str());
			s = name;
			s += '=';
			s += converted;
		}
		strings.push_back(s);
		totalSize += s.size() + 1;
	}

	totalSize++; // For the final null
	return strings;
}

std::string convertEnvValueToHost(const std::string &name, const char *rawValue) {
	if (!rawValue) {
		return {};
	}
	if (strcasecmp(name.c_str(), "PATH") != 0) {
		return rawValue;
	}
	std::string converted = files::windowsPathListToHost(rawValue);
	return converted.empty() ? std::string(rawValue) : converted;
}

} // namespace

namespace kernel32 {
void initializeEnvironment() { ensureTempEnvVariables(); }

GUEST_PTR WINAPI GetCommandLineA() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetCommandLineA() -> %s\n", wibo::commandLine.c_str());
	if (g_commandLineA == GUEST_NULL) {
		void *tmp = wibo::heap::guestMalloc(wibo::commandLine.size() + 1, true);
		memcpy(tmp, wibo::commandLine.c_str(), wibo::commandLine.size());
		g_commandLineA = toGuestPtr(tmp);
	}
	return g_commandLineA;
}

GUEST_PTR WINAPI GetCommandLineW() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetCommandLineW() -> %s\n", wideStringToString(wibo::commandLineW.data()).c_str());
	if (g_commandLineW == GUEST_NULL) {
		void *tmp = wibo::heap::guestMalloc(wibo::commandLineW.size() * sizeof(WCHAR) + sizeof(WCHAR), true);
		memcpy(tmp, wibo::commandLineW.data(), wibo::commandLineW.size() * sizeof(WCHAR));
		g_commandLineW = toGuestPtr(tmp);
	}
	return g_commandLineW;
}

HANDLE WINAPI GetStdHandle(DWORD nStdHandle) {
	HOST_CONTEXT_GUARD();
	HANDLE handle = files::getStdHandle(nStdHandle);
	DEBUG_LOG("GetStdHandle(%d) -> %p\n", nStdHandle, handle);
	return handle;
}

BOOL WINAPI SetStdHandle(DWORD nStdHandle, HANDLE hHandle) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetStdHandle(%d, %p)\n", nStdHandle, hHandle);
	return files::setStdHandle(nStdHandle, hHandle);
}

GUEST_PTR WINAPI GetEnvironmentStrings() { return GetEnvironmentStringsA(); }

GUEST_PTR WINAPI GetEnvironmentStringsA() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetEnvironmentStringsA()\n");

	size_t bufSize = 0;
	auto strings = prepareEnvStrings(bufSize);

	char *buffer = static_cast<char *>(wibo::heap::guestMalloc(bufSize));
	if (!buffer) {
		setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return GUEST_NULL;
	}
	char *ptr = buffer;
	for (const auto &s : strings) {
		memcpy(ptr, s.c_str(), s.size());
		ptr[s.size()] = 0;
		ptr += s.size() + 1;
	}
	*ptr = 0;

	return toGuestPtr(buffer);
}

GUEST_PTR WINAPI GetEnvironmentStringsW() {
	HOST_CONTEXT_GUARD();
	const DWORD incomingError = getLastError();
	DEBUG_LOG("GetEnvironmentStringsW()\n");

	size_t byteSize = 0;
	auto strings = prepareEnvStrings(byteSize);
	std::vector<std::u16string> wideStrings;
	wideStrings.reserve(strings.size());
	size_t totalUnits = 1;
	constexpr size_t maximumUnits = std::numeric_limits<size_t>::max() / sizeof(WCHAR);
	for (const auto &string : strings) {
		auto wide = utf8ToUtf16(string);
		if (!wide) {
			setLastError(kNoUnicodeTranslation);
			return GUEST_NULL;
		}
		if (totalUnits == maximumUnits || wide->size() > maximumUnits - totalUnits - 1) {
			setLastError(ERROR_NOT_ENOUGH_MEMORY);
			return GUEST_NULL;
		}
		totalUnits += wide->size() + 1;
		wideStrings.push_back(std::move(*wide));
	}
	totalUnits = std::max(totalUnits, size_t(2));

	WCHAR *buffer = static_cast<WCHAR *>(wibo::heap::guestMalloc(totalUnits * sizeof(WCHAR), true));
	if (!buffer) {
		setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return GUEST_NULL;
	}
	WCHAR *ptr = buffer;
	for (const auto &string : wideStrings) {
		ptr = std::copy(string.begin(), string.end(), ptr);
		*ptr++ = 0;
	}
	*ptr = 0;
	setLastError(incomingError);
	return toGuestPtr(buffer);
}

BOOL WINAPI FreeEnvironmentStringsA(LPCH penv) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FreeEnvironmentStringsA(%p)\n", penv);
	if (!penv) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	wibo::heap::guestFree(penv);
	return TRUE;
}

BOOL WINAPI FreeEnvironmentStringsW(LPWCH penv) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FreeEnvironmentStringsW(%p)\n", penv);
	if (!penv) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	wibo::heap::guestFree(penv);
	return TRUE;
}

DWORD WINAPI GetEnvironmentVariableA(LPCSTR lpName, LPSTR lpBuffer, DWORD nSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetEnvironmentVariableA(%s, %p, %u)\n", lpName ? lpName : "(null)", lpBuffer, nSize);
	if (!lpName) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	auto value = getEnvValueForWindows(lpName);
	if (!value) {
		setLastError(ERROR_ENVVAR_NOT_FOUND);
		return 0;
	}
	DWORD len = static_cast<DWORD>(value->size());
	if (nSize == 0) {
		return len + 1;
	}
	if (!lpBuffer) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	if (nSize <= len) {
		return len + 1;
	}
	memcpy(lpBuffer, value->c_str(), len + 1);
	return len;
}

DWORD WINAPI GetEnvironmentVariableW(LPCWSTR lpName, LPWSTR lpBuffer, DWORD nSize) {
	HOST_CONTEXT_GUARD();
	const DWORD incomingError = getLastError();
	if (!lpName || !*lpName) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	std::string name;
	if (!utf16ToUtf8(std::u16string(lpName, lpName + wstrlen(lpName)), name)) {
		setLastError(kNoUnicodeTranslation);
		return 0;
	}
	DEBUG_LOG("GetEnvironmentVariableW(%s, %p, %u)\n", name.c_str(), lpBuffer, nSize);
	auto value = getEnvValueForWindows(name);
	if (!value) {
		setLastError(ERROR_ENVVAR_NOT_FOUND);
		return 0;
	}
	auto wideValue = utf8ToUtf16(*value);
	if (!wideValue) {
		setLastError(kNoUnicodeTranslation);
		return 0;
	}
	if (wideValue->size() >= std::numeric_limits<DWORD>::max()) {
		setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return 0;
	}
	DWORD required = static_cast<DWORD>(wideValue->size() + 1);
	if (nSize == 0) {
		setLastError(incomingError);
		return required;
	}
	if (!lpBuffer) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	if (nSize < required) {
		setLastError(incomingError);
		return required;
	}
	std::copy(wideValue->begin(), wideValue->end(), lpBuffer);
	lpBuffer[required - 1] = 0;
	setLastError(incomingError);
	return required - 1;
}

DWORD WINAPI ExpandEnvironmentStringsA(LPCSTR lpSrc, LPSTR lpDst, DWORD nSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("ExpandEnvironmentStringsA(%p, %p, %u)\n", lpSrc, lpDst, nSize);
	if (!lpSrc) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	std::string expanded;
	constexpr size_t maximum = std::numeric_limits<DWORD>::max();
	bool complete = visitEnvironmentExpansion(std::string_view(lpSrc), [&](std::string_view part, bool) {
		if (part.size() > maximum - 2 - expanded.size()) {
			return false;
		}
		expanded.append(part);
		return true;
	});
	if (!complete) {
		setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return 0;
	}
	DWORD required = static_cast<DWORD>(expanded.size() + 1);
	if (!lpDst || nSize <= required) {
		// The documented ANSI buffer rule needs one extra character. Wine
		// reports that larger requirement and empties a non-NULL destination,
		// including a query with nSize == 0 and a valid destination pointer.
		if (lpDst) {
			lpDst[0] = 0;
		}
		return required + 1;
	}
	std::memcpy(lpDst, expanded.c_str(), required);
	return required;
}

DWORD WINAPI ExpandEnvironmentStringsW(LPCWSTR lpSrc, LPWSTR lpDst, DWORD nSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("ExpandEnvironmentStringsW(%p, %p, %u)\n", lpSrc, lpDst, nSize);
	if (!lpSrc || (!lpDst && nSize != 0)) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	// Preserve literal UTF-16 independently of the single-byte environment
	// storage; no wideStringToString conversion is permitted here.
	std::u16string source(lpSrc, lpSrc + wstrlen(lpSrc));
	DWORD remaining = nSize;
	size_t written = 0;
	DWORD required = 1;
	bool complete =
		visitEnvironmentExpansion(std::u16string_view(source), [&](std::u16string_view part, bool variable) {
			if (part.size() > std::numeric_limits<DWORD>::max() - required) {
				return false;
			}
			required += static_cast<DWORD>(part.size());
			if (remaining == 0) {
				return true;
			}
			if (part.size() >= remaining) {
				// Wine's wide API copies a literal prefix without adding a NUL on
				// overflow. A variable value is copied only whole; when it cannot
				// fit, the current position is terminated instead. Continue counting.
				if (variable) {
					lpDst[written] = 0;
				} else {
					std::copy_n(part.begin(), remaining - 1, lpDst + written);
				}
				remaining = 0;
				return true;
			}
			std::copy(part.begin(), part.end(), lpDst + written);
			written += part.size();
			remaining -= static_cast<DWORD>(part.size());
			if (variable) {
				lpDst[written] = 0;
			}
			return true;
		});
	if (!complete) {
		setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return 0;
	}
	if (remaining != 0) {
		lpDst[written] = 0;
	}
	return required;
}

BOOL WINAPI SetEnvironmentVariableA(LPCSTR lpName, LPCSTR lpValue) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SetEnvironmentVariableA(%s, %s)\n", lpName ? lpName : "(null)", lpValue ? lpValue : "(null)");
	if (!lpName || std::strchr(lpName, '=')) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	ensureTempEnvVariables();
	std::string environmentName(lpName);
	for (char **work = environ; *work; ++work) {
		std::string_view entry(*work);
		const auto separator = entry.find('=');
		if (separator == std::string_view::npos)
			continue;
		std::string candidate(entry.substr(0, separator));
		if (strcasecmp(candidate.c_str(), lpName) == 0) {
			environmentName = std::move(candidate);
			break;
		}
	}
	int rc = 0;
	if (!lpValue) {
		rc = unsetenv(environmentName.c_str());
		if (rc != 0) {
			setLastErrorFromErrno();
			return FALSE;
		}
		return TRUE;
	}
	std::string hostValue = convertEnvValueToHost(lpName, lpValue);
	const char *valuePtr = hostValue.empty() ? lpValue : hostValue.c_str();
	rc = setenv(environmentName.c_str(), valuePtr, 1);
	if (rc != 0) {
		setLastErrorFromErrno();
		return FALSE;
	}
	return TRUE;
}

BOOL WINAPI SetEnvironmentVariableW(LPCWSTR lpName, LPCWSTR lpValue) {
	HOST_CONTEXT_GUARD();
	const DWORD incomingError = getLastError();
	DEBUG_LOG("SetEnvironmentVariableW -> ");
	if (!lpName) {
		setLastError(ERROR_INVALID_PARAMETER);
		DEBUG_LOG("ERROR_INVALID_PARAMETER\n");
		return FALSE;
	}
	std::string name, value;
	if (!utf16ToUtf8(std::u16string(lpName, lpName + wstrlen(lpName)), name) ||
		(lpValue && !utf16ToUtf8(std::u16string(lpValue, lpValue + wstrlen(lpValue)), value))) {
		setLastError(kNoUnicodeTranslation);
		return FALSE;
	}
	BOOL result = SetEnvironmentVariableA(name.c_str(), lpValue ? value.c_str() : nullptr);
	if (result)
		setLastError(incomingError);
	return result;
}

} // namespace kernel32
