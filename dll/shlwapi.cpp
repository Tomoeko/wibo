#include "shlwapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "kernel32/fileapi.h"
#include "kernel32/internal.h"
#include "kernel32/minwinbase.h"
#include "modules.h"
#include "strutil.h"
#include "system_provider.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr size_t kMaxPatternUnits = (64 * 1024 - 256) / (2 * sizeof(WCHAR));

WCHAR upperAscii(WCHAR value) { return value >= 'a' && value <= 'z' ? value - ('a' - 'A') : value; }

bool matchesAsciiPattern(std::basic_string_view<WCHAR> file, std::basic_string_view<WCHAR> pattern) {
	size_t filePosition = 0, patternPosition = 0;
	size_t starPosition = decltype(pattern)::npos, retryPosition = 0;
	while (filePosition < file.size()) {
		if (patternPosition < pattern.size() && pattern[patternPosition] == '*') {
			starPosition = patternPosition++;
			retryPosition = filePosition;
		} else if (patternPosition < pattern.size() &&
				   (pattern[patternPosition] == '?' ||
					upperAscii(pattern[patternPosition]) == upperAscii(file[filePosition]))) {
			++filePosition;
			++patternPosition;
		} else if (starPosition != decltype(pattern)::npos) {
			filePosition = ++retryPosition;
			patternPosition = starPosition + 1;
		} else {
			return false;
		}
	}
	while (patternPosition < pattern.size() && pattern[patternPosition] == '*')
		++patternPosition;
	return patternPosition == pattern.size();
}

bool matchesAsciiPatterns(std::basic_string_view<WCHAR> file, std::basic_string_view<WCHAR> patterns) {
	if (patterns.size() == 3 && patterns[0] == '*' && patterns[1] == '.' && patterns[2] == '*')
		return true;
	for (size_t position = 0; position < patterns.size();) {
		const size_t separator = patterns.find(';', position);
		const size_t end = separator == decltype(patterns)::npos ? patterns.size() : separator;
		while (position < end && patterns[position] == ' ')
			++position;
		if (matchesAsciiPattern(file, patterns.substr(position, end - position)))
			return true;
		position = end + 1;
	}
	return false;
}

} // namespace

namespace shlwapi {

BOOL WINAPI PathMatchSpecW(LPCWSTR file, LPCWSTR pattern) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("PathMatchSpecW(%p, %p)\n", file, pattern);
	const DWORD incomingError = kernel32::getLastError();
	if (!file || !pattern) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	const auto validRange = [](const void *pointer) {
		return kMaxPatternUnits * sizeof(WCHAR) <=
			   std::numeric_limits<uintptr_t>::max() - reinterpret_cast<uintptr_t>(pointer);
	};
	if (!validRange(file) || !validRange(pattern)) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	const size_t fileUnits = wstrnlen(file, kMaxPatternUnits);
	const size_t patternUnits = wstrnlen(pattern, kMaxPatternUnits);
	if (fileUnits == kMaxPatternUnits || patternUnits == kMaxPatternUnits ||
		fileUnits + patternUnits > kMaxPatternUnits) {
		kernel32::setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	const std::basic_string_view<WCHAR> fileName(file, fileUnits), patterns(pattern, patternUnits);
	const auto ascii = [](WCHAR unit) { return unit < 128; };
	if (std::all_of(fileName.begin(), fileName.end(), ascii) && std::all_of(patterns.begin(), patterns.end(), ascii)) {
		const BOOL result = matchesAsciiPatterns(fileName, patterns);
		kernel32::setLastError(incomingError);
		return result;
	}
	const auto encode = [](LPCWSTR value, size_t units) {
		return wibo::provider::encodeBytes(
			std::string_view(reinterpret_cast<const char *>(value), units * sizeof(WCHAR)));
	};
	std::vector<uint8_t> response;
	if (!wibo::provider::request({"path-match-spec-w", encode(file, fileUnits), encode(pattern, patternUnits),
								  std::to_string(incomingError)},
								 response)) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	uint32_t result = 0, nativeError = 0;
	if (!reader.header(status) || (status && !reader.done())) {
		kernel32::setLastError(ERROR_INVALID_DATA);
		return FALSE;
	}
	if (status) {
		kernel32::setLastError(status == wibo::provider::kUnavailable ? ERROR_NOT_SUPPORTED
																	  : static_cast<DWORD>(status));
		return FALSE;
	}
	if (!reader.number(result) || result > 1 || !reader.number(nativeError) || !reader.done()) {
		kernel32::setLastError(ERROR_INVALID_DATA);
		return FALSE;
	}
	kernel32::setLastError(nativeError);
	return static_cast<BOOL>(result);
}

BOOL WINAPI PathIsDirectoryW(LPCWSTR path) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("PathIsDirectoryW(%p)\n", path);
	if (!path)
		return FALSE;
	if (!*path) {
		kernel32::setLastError(ERROR_PATH_NOT_FOUND);
		return FALSE;
	}
	const size_t length = wstrnlen(path, MAX_PATH);
	if (length == MAX_PATH || (length >= 2 && path[0] == '\\' && path[1] == '\\')) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	std::string utf8;
	if (!utf16ToUtf8(std::u16string_view(reinterpret_cast<const char16_t *>(path), length), utf8)) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	const DWORD attributes = kernel32::GetFileAttributesA(utf8.c_str());
	return attributes == INVALID_FILE_ATTRIBUTES ? FALSE : static_cast<BOOL>(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

BOOL WINAPI PathIsDirectoryEmptyW(LPCWSTR path) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("PathIsDirectoryEmptyW(%p)\n", path);
	if (!PathIsDirectoryW(path))
		return FALSE;
	const size_t length = wstrnlen(path, MAX_PATH);
	std::string pattern;
	if (length == MAX_PATH ||
		!utf16ToUtf8(std::u16string_view(reinterpret_cast<const char16_t *>(path), length), pattern)) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	if (!pattern.empty() && pattern.back() != '\\' && pattern.back() != '/')
		pattern.push_back('\\');
	pattern.push_back('*');
	WIN32_FIND_DATAA entry{};
	const HANDLE search = kernel32::FindFirstFileA(pattern.c_str(), &entry);
	if (search == INVALID_HANDLE_VALUE)
		return kernel32::getLastError() == ERROR_FILE_NOT_FOUND;
	bool empty = true;
	do {
		if (std::strcmp(entry.cFileName, ".") != 0 && std::strcmp(entry.cFileName, "..") != 0) {
			empty = false;
			break;
		}
	} while (kernel32::FindNextFileA(search, &entry));
	kernel32::FindClose(search);
	return empty;
}

BOOL WINAPI PathIsRelativeW(LPCWSTR path) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("PathIsRelativeW(%p)\n", path);
	if (!path || !*path)
		return FALSE;
	if (path[0] == '\\' || path[0] == '/')
		return FALSE;
	return path[1] != ':';
}

LPCWSTR WINAPI PathFindFileNameW(LPCWSTR path) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("PathFindFileNameW(%p)\n", path);
	if (!path)
		return nullptr;
	LPCWSTR name = path;
	for (LPCWSTR current = path; *current; ++current) {
		if (*current == '\\' || *current == '/' || *current == ':')
			name = current + 1;
	}
	return name;
}

BOOL WINAPI PathFileExistsW(LPCWSTR path) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("PathFileExistsW(%p)\n", path);
	if (!path || !*path)
		return FALSE;
	return kernel32::GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

BOOL WINAPI PathCanonicalizeW(LPWSTR output, LPCWSTR path) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("PathCanonicalizeW(%p, %p)\n", output, path);
	if (!output || !path) {
		if (output)
			output[0] = 0;
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	const size_t length = wstrnlen(path, MAX_PATH);
	if (length == MAX_PATH) {
		output[0] = 0;
		kernel32::setLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	const std::basic_string_view<uint16_t> input(path, length);
	size_t rootLength = 0;
	if (!input.empty() && input[0] == '\\') {
		rootLength = 1;
		if (length > 1 && input[1] == '\\') {
			rootLength = input.find('\\', 2);
			if (rootLength == decltype(input)::npos)
				rootLength = length;
		}
	} else if (length >= 3 && input[1] == ':' && input[2] == '\\')
		rootLength = 3;
	std::basic_string<uint16_t> result(input.substr(0, rootLength));
	for (size_t position = rootLength; position < length;) {
		const bool boundary = position == rootLength || input[position - 1] == '\\';
		if (boundary && input[position] == '.' && position + 1 < length && input[position + 1] == '\\') {
			position += 2;
			continue;
		}
		if (boundary && input[position] == '.' && position + 1 < length && input[position + 1] == '.' &&
			(position + 2 == length || input[position + 2] == '\\') && !result.empty()) {
			if (result.size() > rootLength) {
				if (result.back() == '\\')
					result.pop_back();
				const auto separator = result.rfind('\\');
				result.resize(separator == decltype(result)::npos ? rootLength : std::max(rootLength, separator));
			}
			position += 2;
			if (result.empty())
				result.push_back('\\');
			if (position < length && result.back() == '\\')
				++position;
			continue;
		}
		result.push_back(input[position++]);
	}
	if (result.empty() || (result.size() == 2 && result[1] == ':'))
		result.push_back('\\');
	std::memcpy(output, result.data(), result.size() * sizeof(uint16_t));
	output[result.size()] = 0;
	return TRUE;
}

LPSTR WINAPI PathAddBackslashA(LPSTR pszPath) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("PathAddBackslashA(%s)\n", pszPath ? pszPath : "(null)");
	if (!pszPath) {
		return nullptr;
	}

	size_t length = std::strlen(pszPath);
	if (length > 0 && pszPath[length - 1] == '\\') {
		return pszPath + length;
	}

	if (length + 1 >= MAX_PATH) {
		return nullptr;
	}

	pszPath[length] = '\\';
	pszPath[length + 1] = '\0';
	return pszPath + length + 1;
}

} // namespace shlwapi

#include "shlwapi_trampolines.h"

extern const wibo::ModuleStub lib_shlwapi = {
	(const char *[]){
		"shlwapi",
		nullptr,
	},
	shlwapiThunkByName,
	nullptr,
};
