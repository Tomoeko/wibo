#include "winprofile.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "files.h"
#include "kernel32/internal.h"
#include "kernel32/winbase.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Read-only disk profiles. Registry IniFileMapping, profile writes/cache updates,
// other Windows ANSI code pages, and general Unicode name folding are unsupported.
// The current single-byte ACP is ISO-8859-1; UTF-16LE BOM files retain wide values.
// Paths use wibo's existing Windows path mapper, including WIBO_C_DRIVE.
// https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getprivateprofilestringw
// https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getprivateprofileintw
struct ProfileKey {
	std::u16string name;
	std::u16string value;
};
struct ProfileSection {
	std::u16string name;
	std::vector<ProfileKey> keys;
};
struct Profile {
	std::vector<ProfileSection> sections;
	DWORD error = ERROR_SUCCESS;
};

std::u16string widen(LPCSTR value) {
	std::u16string result;
	if (value) {
		while (*value) {
			result.push_back(static_cast<unsigned char>(*value++));
		}
	}
	return result;
}
std::u16string widen(LPCWSTR value) {
	std::u16string result;
	if (value) {
		while (*value) {
			result.push_back(*value++);
		}
	}
	return result;
}

bool profileSpace(char16_t ch) { return ch == u' ' || ch == u'\t' || ch == u'\r' || ch == u'\n'; }
std::u16string_view trim(std::u16string_view value) {
	while (!value.empty() && profileSpace(value.front())) {
		value.remove_prefix(1);
	}
	while (!value.empty() && profileSpace(value.back())) {
		value.remove_suffix(1);
	}
	return value;
}
char16_t upperName(char16_t ch) {
	if ((ch >= u'a' && ch <= u'z') || (ch >= 0xe0 && ch <= 0xf6) || (ch >= 0xf8 && ch <= 0xfe)) {
		return ch - 0x20;
	}
	return ch == 0xff ? 0x0178 : ch;
}
bool equalName(std::u16string_view a, std::u16string_view b) {
	a = trim(a);
	b = trim(b);
	return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char16_t left, char16_t right) {
			   return upperName(left) == upperName(right);
		   });
}

Profile readProfile(const std::u16string &name) {
	Profile result;
	std::string filename;
	for (char16_t ch : name) {
		if (ch > 0xff) {
			result.error = ERROR_NOT_SUPPORTED;
			return result;
		}
		filename.push_back(static_cast<char>(ch));
	}
	if (filename.find_first_of("/\\:") == std::string::npos) {
		char directory[260];
		UINT length = kernel32::GetWindowsDirectoryA(directory, sizeof(directory));
		if (!length || length >= sizeof(directory)) {
			result.error = ERROR_PATH_NOT_FOUND;
			return result;
		}
		filename = std::string(directory) + "\\" + filename;
	}
	auto path = files::pathFromWindows(filename.c_str());
	DEBUG_LOG("profile read: %s -> %s\n", filename.c_str(), path.c_str());
	std::unique_ptr<FILE, decltype(&std::fclose)> file(std::fopen(path.c_str(), "rb"), &std::fclose);
	if (!file) {
		result.error = ERROR_FILE_NOT_FOUND;
		return result;
	}
	std::string bytes;
	char buffer[4096];
	while (size_t count = std::fread(buffer, 1, sizeof(buffer), file.get())) {
		bytes.append(buffer, count);
	}
	if (std::ferror(file.get())) {
		result.error = ERROR_FILE_NOT_FOUND;
		return result;
	}
	std::u16string text;
	if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xff &&
		static_cast<unsigned char>(bytes[1]) == 0xfe) {
		if (bytes.size() % 2 != 0) {
			result.error = ERROR_NOT_SUPPORTED;
			return result;
		}
		for (size_t offset = 2; offset < bytes.size(); offset += 2) {
			text.push_back(static_cast<unsigned char>(bytes[offset]) |
						   (static_cast<unsigned char>(bytes[offset + 1]) << 8));
		}
	} else {
		if ((bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xfe &&
			 static_cast<unsigned char>(bytes[1]) == 0xff) ||
			(bytes.size() >= 3 && bytes.compare(0, 3, "\xef\xbb\xbf") == 0)) {
			result.error = ERROR_NOT_SUPPORTED;
			return result;
		}
		for (unsigned char byte : bytes) {
			text.push_back(byte);
		}
	}
	if (size_t nul = text.find(u'\0'); nul != std::u16string::npos) {
		text.resize(nul);
	}
	for (size_t offset = 0; offset < text.size();) {
		size_t end = text.find_first_of(u"\r\n", offset);
		if (end == std::u16string::npos) {
			end = text.size();
		}
		auto line = trim(std::u16string_view(text).substr(offset, end - offset));
		offset = end + 1;
		if (line.empty() || line.front() == u';') {
			continue;
		}
		if (line.front() == u'[') {
			size_t close = line.find(u']');
			if (close != std::u16string_view::npos) {
				result.sections.push_back({std::u16string(trim(line.substr(1, close - 1))), {}});
			}
			continue;
		}
		size_t equal = line.find(u'=');
		if (!result.sections.empty() && equal != std::u16string_view::npos) {
			result.sections.back().keys.push_back(
				{std::u16string(trim(line.substr(0, equal))), std::u16string(trim(line.substr(equal + 1)))});
		}
	}
	return result;
}

const ProfileSection *findSection(const Profile &profile, const std::u16string &name) {
	for (const auto &section : profile.sections) {
		if (equalName(section.name, name)) {
			return &section;
		}
	}
	return nullptr;
}
std::optional<std::u16string> findValue(const Profile &profile, const std::u16string &sectionName,
										const std::u16string &keyName) {
	if (const auto *section = findSection(profile, sectionName)) {
		for (const auto &key : section->keys) {
			if (equalName(key.name, keyName)) {
				auto value = key.value;
				if (value.size() >= 2 && (value.front() == u'\'' || value.front() == u'"') &&
					value.back() == value.front()) {
					value = value.substr(1, value.size() - 2);
				}
				return value;
			}
		}
	}
	return std::nullopt;
}

UINT integerValue(const std::optional<std::u16string> &found, int fallback, bool rejectNegative) {
	if (!found || found->empty()) {
		return static_cast<UINT>(fallback);
	}
	auto value = trim(*found);
	bool negative = false;
	if (!value.empty() && (value.front() == u'-' || value.front() == u'+')) {
		negative = value.front() == u'-';
		value.remove_prefix(1);
	}
	UINT base = 10;
	if (value.size() >= 2 && value[0] == u'0' && (value[1] == u'x' || value[1] == u'X')) {
		base = 16;
		value.remove_prefix(2);
	}
	UINT result = 0;
	for (char16_t ch : value) {
		UINT digit = ch >= u'0' && ch <= u'9'	? ch - u'0'
					 : ch >= u'a' && ch <= u'f' ? ch - u'a' + 10
					 : ch >= u'A' && ch <= u'F' ? ch - u'A' + 10
												: base;
		if (digit >= base) {
			break;
		}
		result = result * base + digit; // Deliberate modulo-2^32 behavior observed in Wine.
	}
	return negative ? (rejectNegative ? 0u : 0u - result) : result;
}

template <typename Input>
UINT readInteger(Input app, Input key, int fallback, Input filename, bool rejectNegative = false) {
	auto profile = readProfile(filename ? widen(filename) : u"win.ini");
	kernel32::setLastError(profile.error);
	return app && key ? integerValue(findValue(profile, widen(app), widen(key)), fallback, rejectNegative)
					  : static_cast<UINT>(fallback);
}

template <typename Output> std::basic_string<Output> encode(const std::u16string &value) {
	std::basic_string<Output> result;
	for (char16_t ch : value) {
		if constexpr (sizeof(Output) == 1) {
			result.push_back(ch <= 0xff ? static_cast<char>(ch) : '?');
		} else {
			result.push_back(ch);
		}
	}
	return result;
}

template <typename Input, typename Output>
DWORD readString(Input app, Input key, Input fallback, Output *output, DWORD size, Input filename) {
	if (size == 0 || !output) {
		return 0;
	}
	auto profile = readProfile(filename ? widen(filename) : u"win.ini");
	kernel32::setLastError(profile.error);
	std::u16string value;
	bool multi = false;
	if (!app) {
		for (const auto &section : profile.sections) {
			if (!section.name.empty()) {
				value += section.name;
				value.push_back(0);
			}
		}
		multi = !value.empty();
	} else if (!key) {
		if (const auto *section = findSection(profile, widen(app))) {
			for (const auto &entry : section->keys) {
				value += entry.name;
				value.push_back(0);
			}
		}
		multi = !value.empty();
		if (!multi) {
			value = widen(fallback);
		}
	} else {
		auto found = findValue(profile, widen(app), widen(key));
		value = found ? std::move(*found) : widen(fallback);
		if (!found) {
			while (!value.empty() && value.back() == u' ') {
				value.pop_back();
			}
		}
	}
	auto encoded = encode<Output>(value);
	if (multi && encoded.size() >= size) {
		DWORD count = size > 1 ? size - 2 : 0;
		std::copy_n(encoded.begin(), count, output);
		output[count] = 0;
		if (size > 1) {
			output[count + 1] = 0;
		}
		// Follow the documented double-NUL contract. Wine 11's A conversion
		// leaves the second trailing NUL untouched; the fixture records that
		// discrepancy explicitly rather than treating it as Windows evidence.
		return count;
	}
	DWORD count = static_cast<DWORD>(std::min(encoded.size(), static_cast<size_t>(size - 1)));
	std::copy_n(encoded.begin(), count, output);
	output[count] = 0;
	return count;
}

} // namespace

namespace kernel32 {

UINT WINAPI GetPrivateProfileIntA(LPCSTR lpAppName, LPCSTR lpKeyName, int nDefault, LPCSTR lpFileName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetPrivateProfileIntA(%p, %p, %d, %p)\n", lpAppName, lpKeyName, nDefault, lpFileName);
	return readInteger(lpAppName, lpKeyName, nDefault, lpFileName);
}
UINT WINAPI GetPrivateProfileIntW(LPCWSTR lpAppName, LPCWSTR lpKeyName, int nDefault, LPCWSTR lpFileName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetPrivateProfileIntW(%p, %p, %d, %p)\n", lpAppName, lpKeyName, nDefault, lpFileName);
	return readInteger(lpAppName, lpKeyName, nDefault, lpFileName);
}
DWORD WINAPI GetPrivateProfileStringA(LPCSTR lpAppName, LPCSTR lpKeyName, LPCSTR lpDefault, LPSTR lpReturnedString,
									  DWORD nSize, LPCSTR lpFileName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetPrivateProfileStringA(%p, %p, %p, %p, %u, %p)\n", lpAppName, lpKeyName, lpDefault, lpReturnedString,
			  nSize, lpFileName);
	return readString(lpAppName, lpKeyName, lpDefault, lpReturnedString, nSize, lpFileName);
}
DWORD WINAPI GetPrivateProfileStringW(LPCWSTR lpAppName, LPCWSTR lpKeyName, LPCWSTR lpDefault, LPWSTR lpReturnedString,
									  DWORD nSize, LPCWSTR lpFileName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetPrivateProfileStringW(%p, %p, %p, %p, %u, %p)\n", lpAppName, lpKeyName, lpDefault, lpReturnedString,
			  nSize, lpFileName);
	return readString(lpAppName, lpKeyName, lpDefault, lpReturnedString, nSize, lpFileName);
}
UINT WINAPI GetProfileIntA(LPCSTR lpAppName, LPCSTR lpKeyName, int nDefault) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetProfileIntA(%p, %p, %d)\n", lpAppName, lpKeyName, nDefault);
	// GetProfileInt documents zero for a negative stored value; the private
	// API has no such restriction. Wine 11 wraps for both (fixture records it).
	return readInteger(lpAppName, lpKeyName, nDefault, "win.ini", true);
}
UINT WINAPI GetProfileIntW(LPCWSTR lpAppName, LPCWSTR lpKeyName, int nDefault) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetProfileIntW(%p, %p, %d)\n", lpAppName, lpKeyName, nDefault);
	static const WCHAR filename[] = {'w', 'i', 'n', '.', 'i', 'n', 'i', 0};
	return readInteger(lpAppName, lpKeyName, nDefault, filename, true);
}
DWORD WINAPI GetProfileStringA(LPCSTR lpAppName, LPCSTR lpKeyName, LPCSTR lpDefault, LPSTR lpReturnedString,
							   DWORD nSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetProfileStringA(%p, %p, %p, %p, %u)\n", lpAppName, lpKeyName, lpDefault, lpReturnedString, nSize);
	return readString(lpAppName, lpKeyName, lpDefault, lpReturnedString, nSize, "win.ini");
}
DWORD WINAPI GetProfileStringW(LPCWSTR lpAppName, LPCWSTR lpKeyName, LPCWSTR lpDefault, LPWSTR lpReturnedString,
							   DWORD nSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetProfileStringW(%p, %p, %p, %p, %u)\n", lpAppName, lpKeyName, lpDefault, lpReturnedString, nSize);
	static const WCHAR filename[] = {'w', 'i', 'n', '.', 'i', 'n', 'i', 0};
	return readString(lpAppName, lpKeyName, lpDefault, lpReturnedString, nSize, filename);
}

} // namespace kernel32
