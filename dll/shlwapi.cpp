#include "shlwapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "kernel32/internal.h"
#include "kernel32/minwinbase.h"
#include "modules.h"
#include "strutil.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>

namespace shlwapi {

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
