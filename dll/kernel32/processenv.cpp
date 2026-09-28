#include "processenv.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "files.h"
#include "heap.h"
#include "internal.h"
#include "strutil.h"
#include "system_provider.h"
#include "types.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <mimalloc.h>
#include <mutex>
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

char16_t foldEnvironmentCharacter(char16_t character) {
	// Keep the current ASCII/Latin-1 case mapping independent of the host locale.
	// Other Unicode case mappings remain outside this name comparison's verified scope.
	if ((character >= u'a' && character <= u'z') || (character >= 0x00e0 && character <= 0x00f6) ||
		(character >= 0x00f8 && character <= 0x00fe))
		return character - 0x20;
	if (character == 0x00ff)
		return 0x0178;
	return character;
}

struct EnvironmentNameLess {
	bool operator()(std::u16string_view left, std::u16string_view right) const {
		const size_t common = std::min(left.size(), right.size());
		for (size_t index = 0; index < common; ++index) {
			const char16_t first = foldEnvironmentCharacter(left[index]);
			const char16_t second = foldEnvironmentCharacter(right[index]);
			if (first != second)
				return first < second;
		}
		return left.size() < right.size();
	}
	using is_transparent = void;
};

using Environment = std::map<std::u16string, std::u16string, EnvironmentNameLess>;
std::mutex g_environmentMutex;
std::once_flag g_environmentInitialization;
Environment g_environment;
bool g_environmentInitialized = false;
constexpr size_t kMaxEnvironmentUnits = 2 * 1024 * 1024;

const std::vector<std::u16string> &environmentBridgeNames() {
	static const std::vector<std::u16string> names = [] {
		std::vector<std::u16string> result;
		const char *setting = std::getenv("WIBO_GUEST_ENVIRONMENT_BRIDGE");
		if (!setting || !*setting)
			return result;
		std::string_view remaining(setting);
		if (remaining.size() > 65536)
			return result;
		for (;;) {
			const size_t comma = remaining.find(',');
			const auto name = remaining.substr(0, comma);
			if (name.empty() || std::any_of(name.begin(), name.end(), [](unsigned char character) {
					return character < 0x21 || character > 0x7e || character == '=';
				})) {
				DEBUG_LOG("Invalid guest environment bridge names; bridge disabled\n");
				return std::vector<std::u16string>{};
			}
			std::u16string wide;
			for (unsigned char character : name)
				wide.push_back(character);
			// A guest cannot change the capability inherited by another host runtime.
			if (!EnvironmentNameLess{}(wide, u"WIBO_GUEST_ENVIRONMENT_BRIDGE") &&
				!EnvironmentNameLess{}(u"WIBO_GUEST_ENVIRONMENT_BRIDGE", wide)) {
				DEBUG_LOG("Guest environment bridge cannot include its control variable\n");
				return std::vector<std::u16string>{};
			}
			result.push_back(std::move(wide));
			if (comma == std::string_view::npos)
				return result;
			remaining.remove_prefix(comma + 1);
		}
	}();
	return names;
}

std::u16string decodeAnsi(std::string_view input) {
	std::u16string output;
	output.reserve(input.size());
	for (unsigned char character : input)
		output.push_back(character);
	return output;
}

std::string encodeAnsi(std::u16string_view input) {
	// GetACP currently selects ISO-8859-1. Unrepresentable characters use its default byte.
	std::string output;
	output.reserve(input.size());
	for (size_t index = 0; index < input.size(); ++index) {
		char16_t character = input[index];
		if (character <= 0xff) {
			output.push_back(static_cast<char>(character));
		} else {
			output.push_back('?');
			if (character >= 0xd800 && character <= 0xdbff && index + 1 < input.size() && input[index + 1] >= 0xdc00 &&
				input[index + 1] <= 0xdfff)
				++index;
		}
	}
	return output;
}

void importEnvironmentDefaults(Environment &environment) {
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
	for (const auto &[name, value] : values) {
		auto wideName = utf8ToUtf16(name);
		auto wideValue = utf8ToUtf16(value);
		if (wideName && wideValue)
			environment.try_emplace(std::move(*wideName), std::move(*wideValue));
	}
}

void ensureEnvironment() {
	(void)environmentBridgeNames();
	std::call_once(g_environmentInitialization, [] {
		{
			std::lock_guard lock(g_environmentMutex);
			if (g_environmentInitialized)
				return;
		}
		Environment initial;
		for (char **entry = environ; *entry; ++entry) {
			std::string_view text(*entry);
			const size_t separator = text.find('=', text.starts_with('=') ? 1 : 0);
			if (separator == std::string_view::npos || separator == 0)
				continue;
			std::string name(text.substr(0, separator));
			auto wideName = utf8ToUtf16(name);
			auto wideValue = utf8ToUtf16(convertEnvValueForWindows(name, text.data() + separator + 1));
			if (wideName && wideValue)
				initial.try_emplace(std::move(*wideName), std::move(*wideValue));
		}
		importEnvironmentDefaults(initial);
		const char *hostTemp = std::getenv("TMPDIR");
		if (!hostTemp || !*hostTemp)
			hostTemp = "/tmp";
		auto temporary = utf8ToUtf16(convertEnvValueForWindows("TEMP", hostTemp));
		if (temporary) {
			initial.try_emplace(u"TMP", *temporary);
			initial.try_emplace(u"TEMP", *temporary);
		}
		std::lock_guard lock(g_environmentMutex);
		if (!g_environmentInitialized) {
			g_environment = std::move(initial);
			g_environmentInitialized = true;
		}
	});
}

Environment environmentSnapshot() {
	ensureEnvironment();
	std::lock_guard lock(g_environmentMutex);
	return g_environment;
}

std::optional<std::u16string> getEnvironmentValue(std::u16string_view name) {
	ensureEnvironment();
	std::lock_guard lock(g_environmentMutex);
	const auto entry = g_environment.find(name);
	return entry == g_environment.end() ? std::nullopt : std::make_optional(entry->second);
}

std::u16string expansionVariableName(std::string_view name) { return decodeAnsi(name); }
std::u16string expansionVariableName(std::u16string_view name) { return std::u16string(name); }

std::string expansionValue(std::u16string_view value, const char *) { return encodeAnsi(value); }
std::u16string expansionValue(std::u16string_view value, const char16_t *) { return std::u16string(value); }

DWORD parseEnvironmentBlock(std::span<const uint16_t> block, Environment &environment) {
	if (block.size() < 2 || block.size() > kMaxEnvironmentUnits || block[block.size() - 1] || block[block.size() - 2])
		return ERROR_INVALID_PARAMETER;
	size_t cursor = 0;
	while (cursor < block.size() && block[cursor]) {
		const size_t start = cursor;
		while (cursor < block.size() && block[cursor])
			++cursor;
		if (cursor == block.size())
			return ERROR_INVALID_PARAMETER;
		const auto units = block.subspan(start, cursor - start);
		std::u16string entry(units.begin(), units.end());
		const size_t separator = entry.find(u'=', entry.starts_with(u'=') ? 1 : 0);
		if (separator == std::u16string::npos || separator == 0)
			return ERROR_INVALID_PARAMETER;
		// Duplicate names have no established selection policy in this parser.
		if (!environment.emplace(entry.substr(0, separator), entry.substr(separator + 1)).second)
			return ERROR_NOT_SUPPORTED;
		++cursor;
	}
	if (cursor + (cursor == 0 ? 2 : 1) != block.size())
		return ERROR_INVALID_PARAMETER;
	return ERROR_SUCCESS;
}

bool appendEnvironmentBlock(const Environment &environment, std::vector<uint16_t> &output) {
	output.clear();
	size_t units = environment.empty() ? 2 : 1;
	for (const auto &[name, value] : environment) {
		if (name.size() > kMaxEnvironmentUnits - 2 || value.size() > kMaxEnvironmentUnits - name.size() - 2 ||
			units > kMaxEnvironmentUnits - name.size() - value.size() - 2)
			return false;
		units += name.size() + value.size() + 2;
	}
	output.reserve(units);
	for (const auto &[name, value] : environment) {
		output.insert(output.end(), name.begin(), name.end());
		output.push_back(u'=');
		output.insert(output.end(), value.begin(), value.end());
		output.push_back(0);
	}
	if (output.empty())
		output.push_back(0);
	output.push_back(0);
	return true;
}

DWORD setEnvironmentValue(std::u16string_view name, const std::optional<std::u16string> &value) {
	if (name.empty() || name.find(u'=') != std::u16string_view::npos)
		return ERROR_INVALID_PARAMETER;
	ensureEnvironment();
	std::lock_guard lock(g_environmentMutex);
	const auto existing = g_environment.find(name);
	if (value) {
		size_t units = 1;
		for (const auto &[key, current] : g_environment)
			if (EnvironmentNameLess{}(key, name) || EnvironmentNameLess{}(name, key)) {
				if (key.size() > kMaxEnvironmentUnits - 2 || current.size() > kMaxEnvironmentUnits - key.size() - 2 ||
					units > kMaxEnvironmentUnits - key.size() - current.size() - 2)
					return ERROR_NOT_ENOUGH_MEMORY;
				units += key.size() + current.size() + 2;
			}
		if (name.size() > kMaxEnvironmentUnits - 2 || value->size() > kMaxEnvironmentUnits - name.size() - 2 ||
			units > kMaxEnvironmentUnits - name.size() - value->size() - 2)
			return ERROR_NOT_ENOUGH_MEMORY;
	}
	for (const auto &allowed : environmentBridgeNames()) {
		if (EnvironmentNameLess{}(allowed, name) || EnvironmentNameLess{}(name, allowed))
			continue;
		std::string hostName(allowed.begin(), allowed.end()), hostValue;
		if (value && !utf16ToUtf8(*value, hostValue))
			return kNoUnicodeTranslation;
		const int result = value ? setenv(hostName.c_str(), hostValue.c_str(), 1) : unsetenv(hostName.c_str());
		if (result)
			return wibo::winErrorFromErrno(errno);
		break;
	}
	if (!value) {
		if (existing != g_environment.end())
			g_environment.erase(existing);
	} else if (existing != g_environment.end()) {
		existing->second = *value;
	} else {
		g_environment.emplace(name, *value);
	}
	return ERROR_SUCCESS;
}

template <typename Character, typename Append>
bool visitEnvironmentExpansion(std::basic_string_view<Character> source, Append append) {
	using View = std::basic_string_view<Character>;
	constexpr Character percent = static_cast<Character>('%');
	const auto environment = environmentSnapshot();
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
		auto entry = name.empty() ? environment.end() : environment.find(name);
		if (entry != environment.end()) {
			auto replacement = expansionValue(entry->second, static_cast<const Character *>(nullptr));
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

} // namespace

namespace kernel32 {
void initializeEnvironment() { ensureEnvironment(); }

std::optional<std::u16string> environmentValue(std::u16string_view name) { return getEnvironmentValue(name); }

DWORD snapshotChildEnvironment(const void *block, bool unicode, std::vector<uint16_t> &output) {
	output.clear();
	if (!block) {
		return appendEnvironmentBlock(environmentSnapshot(), output) ? ERROR_SUCCESS : ERROR_NOT_ENOUGH_MEMORY;
	}
	std::vector<uint16_t> copied;
	for (size_t index = 0; index < kMaxEnvironmentUnits; ++index) {
		uint16_t unit;
		if (unicode)
			std::memcpy(&unit, static_cast<const unsigned char *>(block) + index * sizeof(unit), sizeof(unit));
		else
			unit = static_cast<const unsigned char *>(block)[index];
		copied.push_back(unit);
		if (index && !unit && !copied[index - 1]) {
			Environment parsed;
			const DWORD error = parseEnvironmentBlock(copied, parsed);
			if (!error)
				output = std::move(copied);
			return error;
		}
	}
	return ERROR_NOT_ENOUGH_MEMORY;
}

DWORD installChildEnvironment(std::span<const uint16_t> block) {
	(void)environmentBridgeNames();
	Environment parsed;
	const DWORD error = parseEnvironmentBlock(block, parsed);
	if (error)
		return error;
	std::lock_guard lock(g_environmentMutex);
	g_environment = std::move(parsed);
	g_environmentInitialized = true;
	return ERROR_SUCCESS;
}

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

	const DWORD incomingError = getLastError();
	std::vector<uint16_t> snapshot;
	if (!appendEnvironmentBlock(environmentSnapshot(), snapshot)) {
		setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return GUEST_NULL;
	}
	std::u16string wide(snapshot.begin(), snapshot.end());
	const auto bytes = encodeAnsi(wide);
	const size_t bufSize = bytes.size();

	char *buffer = static_cast<char *>(wibo::heap::guestMalloc(bufSize));
	if (!buffer) {
		setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return GUEST_NULL;
	}
	std::copy(bytes.begin(), bytes.end(), buffer);
	setLastError(incomingError);
	return toGuestPtr(buffer);
}

GUEST_PTR WINAPI GetEnvironmentStringsW() {
	HOST_CONTEXT_GUARD();
	const DWORD incomingError = getLastError();
	DEBUG_LOG("GetEnvironmentStringsW()\n");

	std::vector<uint16_t> snapshot;
	if (!appendEnvironmentBlock(environmentSnapshot(), snapshot)) {
		setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return GUEST_NULL;
	}
	const size_t totalUnits = snapshot.size();

	WCHAR *buffer = static_cast<WCHAR *>(wibo::heap::guestMalloc(totalUnits * sizeof(WCHAR), true));
	if (!buffer) {
		setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return GUEST_NULL;
	}
	std::memcpy(buffer, snapshot.data(), totalUnits * sizeof(WCHAR));
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
	const DWORD incomingError = getLastError();
	if (!lpName || !*lpName) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	auto wideValue = getEnvironmentValue(decodeAnsi(lpName));
	if (!wideValue) {
		setLastError(ERROR_ENVVAR_NOT_FOUND);
		return 0;
	}
	const auto value = encodeAnsi(*wideValue);
	DWORD len = static_cast<DWORD>(value.size());
	if (nSize == 0) {
		setLastError(incomingError);
		return len + 1;
	}
	if (!lpBuffer) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	if (nSize <= len) {
		setLastError(incomingError);
		return len + 1;
	}
	memcpy(lpBuffer, value.c_str(), len + 1);
	setLastError(incomingError);
	return len;
}

DWORD WINAPI GetEnvironmentVariableW(LPCWSTR lpName, LPWSTR lpBuffer, DWORD nSize) {
	HOST_CONTEXT_GUARD();
	const DWORD incomingError = getLastError();
	if (!lpName || !*lpName) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	DEBUG_LOG("GetEnvironmentVariableW(%p, %p, %u)\n", lpName, lpBuffer, nSize);
	auto wideValue =
		getEnvironmentValue(std::u16string_view(reinterpret_cast<const char16_t *>(lpName), wstrlen(lpName)));
	if (!wideValue) {
		setLastError(ERROR_ENVVAR_NOT_FOUND);
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
	// Both the source and owned environment values preserve literal UTF-16.
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
	const DWORD incomingError = getLastError();
	if (!lpName) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	const DWORD error =
		setEnvironmentValue(decodeAnsi(lpName), lpValue ? std::make_optional(decodeAnsi(lpValue)) : std::nullopt);
	if (error) {
		setLastError(error);
		return FALSE;
	}
	setLastError(incomingError);
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
	const std::u16string name(lpName, lpName + wstrlen(lpName));
	const auto value = lpValue ? std::make_optional(std::u16string(lpValue, lpValue + wstrlen(lpValue))) : std::nullopt;
	const DWORD error = setEnvironmentValue(name, value);
	setLastError(error ? error : incomingError);
	return error == ERROR_SUCCESS;
}

DWORD WINAPI SearchPathW(LPCWSTR lpPath, LPCWSTR lpFileName, LPCWSTR lpExtension, DWORD nBufferLength, LPWSTR lpBuffer,
						 GUEST_PTR *lpFilePart) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SearchPathW(%p, %p, %p, %u)\n", lpPath, lpFileName, lpExtension, nBufferLength);
	if (lpFilePart)
		*lpFilePart = GUEST_NULL;
	if (!lpFileName || !*lpFileName || (nBufferLength && !lpBuffer)) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}

	std::u16string name(reinterpret_cast<const char16_t *>(lpFileName), wstrlen(lpFileName));
	const size_t separator = name.find_last_of(u"\\/");
	const size_t dot = name.find_last_of(u'.');
	if (lpExtension && *lpExtension &&
		(dot == std::u16string::npos || (separator != std::u16string::npos && dot < separator))) {
		name.append(reinterpret_cast<const char16_t *>(lpExtension), wstrlen(lpExtension));
	}
	std::string utf8Name;
	if (!utf16ToUtf8(name, utf8Name)) {
		setLastError(kNoUnicodeTranslation);
		return 0;
	}

	std::vector<std::filesystem::path> directories;
	if (lpFileName[0] && (wstrchr(lpFileName, '\\') || wstrchr(lpFileName, '/') || wstrchr(lpFileName, ':'))) {
		directories.emplace_back();
	} else if (lpPath) {
		std::u16string_view paths(reinterpret_cast<const char16_t *>(lpPath), wstrlen(lpPath));
		for (size_t start = 0; start <= paths.size();) {
			const size_t end = paths.find(u';', start);
			const auto entry = paths.substr(start, end == std::u16string_view::npos ? end : end - start);
			std::string utf8Entry;
			if (!utf16ToUtf8(entry, utf8Entry)) {
				setLastError(kNoUnicodeTranslation);
				return 0;
			}
			directories.push_back(utf8Entry.empty() ? std::filesystem::current_path()
													: files::pathFromWindows(utf8Entry.c_str()));
			if (end == std::u16string_view::npos)
				break;
			start = end + 1;
		}
	} else {
		if (wibo::guestExecutablePath.has_parent_path())
			directories.push_back(wibo::guestExecutablePath.parent_path());
		directories.push_back(std::filesystem::current_path());
		const auto system = files::systemSearchDirectories();
		for (const auto &directory : {system.system, system.legacySystem, system.windows})
			if (!directory.empty())
				directories.push_back(directory);
		if (const auto value = getEnvironmentValue(u"PATH")) {
			std::u16string_view paths(*value);
			for (size_t start = 0; start <= paths.size();) {
				const size_t end = paths.find(u';', start);
				const auto entry = paths.substr(start, end == std::u16string_view::npos ? end : end - start);
				std::string utf8Entry;
				if (utf16ToUtf8(entry, utf8Entry) && !utf8Entry.empty())
					directories.push_back(files::pathFromWindows(utf8Entry.c_str()));
				if (end == std::u16string_view::npos)
					break;
				start = end + 1;
			}
		}
	}

	for (const auto &directory : directories) {
		const auto candidate =
			files::pathFromWindows((directory.empty() ? utf8Name : files::pathToWindows(directory / utf8Name)).c_str());
		std::error_code ec;
		if (!std::filesystem::is_regular_file(candidate, ec))
			continue;
		const auto absolute = std::filesystem::absolute(candidate, ec);
		if (ec)
			continue;
		const auto result = utf8ToUtf16(files::pathToWindows(absolute));
		if (!result)
			continue;
		const auto required = static_cast<DWORD>(result->size() + 1);
		if (required > nBufferLength) {
			setLastError(ERROR_INSUFFICIENT_BUFFER);
			return required;
		}
		std::copy(result->begin(), result->end(), lpBuffer);
		lpBuffer[result->size()] = 0;
		if (lpFilePart) {
			const auto last = result->find_last_of(u"\\/");
			*lpFilePart = toGuestPtr(lpBuffer + (last == std::u16string::npos ? 0 : last + 1));
		}
		return required - 1;
	}

	setLastError(ERROR_FILE_NOT_FOUND);
	return 0;
}

} // namespace kernel32
