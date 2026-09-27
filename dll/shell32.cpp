#include "shell32.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "files.h"
#include "kernel32/internal.h"
#include "kernel32/libloaderapi.h"
#include "kernel32/minwinbase.h"
#include "kernel32/processenv.h"
#include "kernel32/winbase.h"
#include "kernel32/winprofile.h"
#include "modules.h"
#include "ole32.h"
#include "strutil.h"
#include "system_provider.h"

#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

std::string folderEnvironment(const char *name, const char *fallback = "") {
	char path[32768];
	const DWORD length = kernel32::GetEnvironmentVariableA(name, path, sizeof(path));
	return length && length < sizeof(path) ? std::string(path, length) : std::string(fallback);
}

std::optional<std::string> specialFolder(int id, bool useDefault) {
	const std::string profile = folderEnvironment("USERPROFILE");
	const auto userFolder = [&](const char *environment, const char *suffix) -> std::optional<std::string> {
		if (!useDefault) {
			auto current = folderEnvironment(environment);
			if (!current.empty()) {
				return current;
			}
		}
		return profile.empty() ? std::nullopt : std::make_optional(profile + suffix);
	};
	switch (id) {
	case 0x05:
		return userFolder("", "\\Documents");
	case 0x1A:
		return userFolder("APPDATA", "\\AppData\\Roaming");
	case 0x1C:
		return userFolder("LOCALAPPDATA", "\\AppData\\Local");
	case 0x23:
		return useDefault ? "C:\\ProgramData" : folderEnvironment("ProgramData", "C:\\ProgramData");
	case 0x24:
		return folderEnvironment("SystemRoot", "C:\\Windows");
	case 0x25:
		return folderEnvironment("SystemRoot", "C:\\Windows") + "\\System32";
	case 0x26:
		return folderEnvironment("ProgramFiles", "C:\\Program Files");
	case 0x28:
		return profile.empty() ? std::nullopt : std::make_optional(profile);
	case 0x2B:
		return folderEnvironment("CommonProgramFiles", "C:\\Program Files\\Common Files");
	default:
		return std::nullopt;
	}
}

bool isArgumentSeparator(uint16_t ch) { return ch == ' ' || ch == '\t'; }

bool supportsShellPath(std::string_view name) {
	if (name.starts_with("\\") || name.starts_with("//") || name.find_first_of("*?") != std::string_view::npos)
		return false;
	const auto colon = name.find(':');
	if (colon == std::string_view::npos)
		return true;
	return colon == 1 && name.size() >= 3 && (name[2] == '/' || name[2] == '\\') &&
		   (name[0] == 'C' || name[0] == 'c' || name[0] == 'Z' || name[0] == 'z') &&
		   name.find(':', 2) == std::string_view::npos;
}

bool isProgramExtension(const std::filesystem::path &path) {
	std::string extension = path.extension().string();
	if (extension.empty())
		return false;
	extension.erase(0, 1);
	toLowerInPlace(extension);
	WCHAR programs[256];
	const DWORD length =
		kernel32::GetProfileStringW(reinterpret_cast<LPCWSTR>(u"windows"), reinterpret_cast<LPCWSTR>(u"programs"),
									reinterpret_cast<LPCWSTR>(u"exe pif bat cmd com"), programs, std::size(programs));
	if (length >= std::size(programs) - 1)
		return false;
	size_t start = 0;
	while (start < length) {
		while (start < length && isArgumentSeparator(programs[start]))
			++start;
		size_t end = start;
		while (end < length && !isArgumentSeparator(programs[end]))
			++end;
		if (end - start == extension.size() &&
			std::equal(extension.begin(), extension.end(), programs + start,
					   [](unsigned char left, WCHAR right) { return left == wcharToLower(right); }))
			return true;
		start = end;
	}
	return false;
}

DWORD missingPathError(const std::filesystem::path &path) {
	std::error_code error;
	const auto parent = std::filesystem::status(path.parent_path(), error);
	return !error && std::filesystem::is_directory(parent) ? ERROR_FILE_NOT_FOUND : ERROR_PATH_NOT_FOUND;
}

std::vector<std::u16string> parseArguments(LPCWSTR command) {
	std::vector<std::u16string> arguments(1);
	// The executable name uses literal backslashes and a single optional pair of quotes.
	if (*command == '"') {
		++command;
		while (*command && *command != '"') {
			arguments.back().push_back(*command++);
		}
		if (*command) {
			++command;
		}
	} else {
		while (*command && !isArgumentSeparator(*command)) {
			arguments.back().push_back(*command++);
		}
	}
	while (*command) {
		while (isArgumentSeparator(*command)) {
			++command;
		}
		if (!*command) {
			break;
		}
		auto &argument = arguments.emplace_back();
		bool quoted = false;
		while (*command && (quoted || !isArgumentSeparator(*command))) {
			size_t slashes = 0;
			while (*command == '\\') {
				++slashes;
				++command;
			}
			if (*command != '"') {
				argument.append(slashes, '\\');
				if (!*command || (!quoted && isArgumentSeparator(*command))) {
					break;
				}
				argument.push_back(*command++);
				continue;
			}
			argument.append(slashes / 2, '\\');
			if (slashes % 2) {
				argument.push_back('"');
				++command;
			} else {
				size_t quotes = quoted ? 1 : 0;
				while (*command == '"') {
					++quotes;
					++command;
				}
				argument.append(quotes / 3, '"');
				quoted = quotes % 3 == 1;
			}
		}
	}
	return arguments;
}

} // namespace

namespace shell32 {

HINSTANCE WINAPI FindExecutableW(LPCWSTR file, LPCWSTR directory, LPWSTR result) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FindExecutableW(%p, %p, %p)\n", file, directory, result);
	const DWORD incomingError = kernel32::getLastError();
	if (!result) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return NO_HANDLE;
	}
	*result = 0;
	if (!file)
		return ERROR_FILE_NOT_FOUND;
	// Registered application paths, extension search, and document associations require shell context.
	const auto unsupported = [] {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return NO_HANDLE;
	};
	std::string filename;
	if (!*file || !utf16ToUtf8(std::u16string_view(reinterpret_cast<const char16_t *>(file), wstrlen(file)), filename))
		return unsupported();
	if (!supportsShellPath(filename) || filename.find_first_of("/\\:") == std::string::npos)
		return unsupported();
	std::error_code error;
	const auto cwd = std::filesystem::current_path(error);
	if (error) {
		kernel32::setLastError(wibo::winErrorFromErrno(error.value()));
		return ERROR_FILE_NOT_FOUND;
	}
	auto base = cwd;
	DWORD successError = incomingError;
	if (directory) {
		std::string directoryName;
		if (!utf16ToUtf8(std::u16string_view(reinterpret_cast<const char16_t *>(directory), wstrlen(directory)),
						 directoryName))
			return unsupported();
		if (!supportsShellPath(directoryName))
			return unsupported();
		if (directoryName.empty()) {
			successError = ERROR_INVALID_NAME;
		} else {
			auto candidate = files::pathFromWindows(directoryName.c_str());
			if (candidate.is_relative())
				candidate = cwd / candidate;
			const auto status = std::filesystem::status(candidate, error);
			if (!error && std::filesystem::is_directory(status))
				base = candidate.lexically_normal();
			else
				successError = ERROR_FILE_NOT_FOUND;
		}
	}
	auto path = files::pathFromWindows(filename.c_str());
	if (path.extension().empty())
		return unsupported();
	if (path.is_relative()) {
		if (directory)
			return unsupported();
		path = base / path;
	}
	path = path.lexically_normal();
	const auto status = std::filesystem::status(path, error);
	if (error || !std::filesystem::exists(status)) {
		if (error && error.value() != ENOENT && error.value() != ENOTDIR) {
			const DWORD statusError = wibo::winErrorFromErrno(error.value());
			kernel32::setLastError(statusError);
			return statusError == ERROR_ACCESS_DENIED ? ERROR_ACCESS_DENIED : ERROR_FILE_NOT_FOUND;
		}
		kernel32::setLastError(missingPathError(path));
		return ERROR_FILE_NOT_FOUND;
	}
	if (!std::filesystem::is_regular_file(status) || !isProgramExtension(path))
		return unsupported();
	const auto wide = utf8ToUtf16(files::pathToWindows(path));
	if (!wide || wide->size() >= MAX_PATH)
		return unsupported();
	std::memcpy(result, wide->c_str(), (wide->size() + 1) * sizeof(WCHAR));
	kernel32::setLastError(successError);
	return 33;
}

HRESULT WINAPI SHGetFolderPathW(HWND hwnd, int csidl, HANDLE hToken, DWORD dwFlags, LPWSTR pszPath) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SHGetFolderPathW(%p, 0x%x, %p, %u, %p)\n", hwnd, csidl, hToken, dwFlags, pszPath);
	if (!pszPath) {
		return static_cast<HRESULT>(0x80070057);
	}
	*pszPath = 0;
	if (dwFlags > 1 || (csidl & ~0xC0FF)) {
		return static_cast<HRESULT>(0x80070057);
	}
	if (hToken) {
		return static_cast<HRESULT>(0x80004001);
	}
	const auto path = specialFolder(csidl & 0xFF, dwFlags == 1);
	if (!path) {
		return static_cast<HRESULT>(0x80070057);
	}
	auto hostPath = files::pathFromWindows(path->c_str());
	std::error_code error;
	if (csidl & 0x8000) {
		std::filesystem::create_directories(hostPath, error);
	}
	if (error || (!(csidl & 0x4000) && !std::filesystem::is_directory(hostPath, error))) {
		return static_cast<HRESULT>(0x80070003);
	}
	auto normalized = *path;
	while (!normalized.empty() && (normalized.back() == '\\' || normalized.back() == '/')) {
		normalized.pop_back();
	}
	const auto wide = stringToWideString(normalized.c_str());
	if (wide.size() > MAX_PATH) {
		return static_cast<HRESULT>(0x8007007A);
	}
	std::copy(wide.begin(), wide.end(), pszPath);
	return S_OK;
}

HRESULT WINAPI SHGetKnownFolderPath(const GUID *id, DWORD flags, HANDLE token, GUEST_PTR *output) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SHGetKnownFolderPath(%p, 0x%x, %p, %p)\n", id, flags, token, output);
	constexpr HRESULT invalidArgument = static_cast<HRESULT>(0x80070057);
	constexpr HRESULT notImplemented = static_cast<HRESULT>(0x80004001);
	constexpr HRESULT invalidData = static_cast<HRESULT>(0x8007000d);
	if (!output)
		return invalidArgument;
	*output = GUEST_NULL;
	if (!id)
		return invalidArgument;
	// Access-token handles cannot be transferred to the adapter process.
	if (token && token != -1)
		return notImplemented;
	const auto identity =
		wibo::provider::encodeBytes(std::string_view(reinterpret_cast<const char *>(id), sizeof(*id)));
	std::vector<uint8_t> response;
	if (!wibo::provider::request(
			{"known-folder-path", identity, std::to_string(flags), token == -1 ? "default" : "current"}, response))
		return notImplemented;
	wibo::provider::Reader reader(response);
	int32_t status = 0;
	if (!reader.header(status))
		return invalidData;
	if (status)
		return reader.done() ? static_cast<HRESULT>(status) : invalidData;
	std::u16string path;
	if (!reader.text(path) || path.empty() || path.find(u'\0') != std::u16string::npos || !reader.done())
		return invalidData;
	const size_t bytes = (path.size() + 1) * sizeof(WCHAR);
	auto *result = static_cast<WCHAR *>(ole32::CoTaskMemAlloc(bytes));
	if (!result)
		return static_cast<HRESULT>(0x8007000e);
	std::memcpy(result, path.c_str(), bytes);
	*output = toGuestPtr(result);
	return S_OK;
}

GUEST_PTR *WINAPI CommandLineToArgvW(LPCWSTR lpCmdLine, int *pNumArgs) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("CommandLineToArgvW(%p, %p)\n", lpCmdLine, pNumArgs);
	if (!lpCmdLine || !pNumArgs) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return nullptr;
	}
	std::vector<std::u16string> arguments;
	if (*lpCmdLine) {
		arguments = parseArguments(lpCmdLine);
	} else {
		std::vector<uint16_t> path(32768);
		const DWORD length = kernel32::GetModuleFileNameW(NO_HANDLE, path.data(), static_cast<DWORD>(path.size()));
		if (!length || length == path.size()) {
			return nullptr;
		}
		arguments.emplace_back(path.begin(), path.begin() + length);
	}
	size_t bytes = (arguments.size() + 1) * sizeof(GUEST_PTR);
	for (const auto &argument : arguments) {
		bytes += (argument.size() + 1) * sizeof(uint16_t);
	}
	// LocalAlloc currently accepts a 32-bit allocation size in the shared heap implementation.
	if (bytes > std::numeric_limits<UINT>::max() || arguments.size() > std::numeric_limits<int>::max()) {
		kernel32::setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return nullptr;
	}
	const HLOCAL block = kernel32::LocalAlloc(0, static_cast<SIZE_T>(bytes));
	if (!block) {
		return nullptr;
	}
	auto *argv = reinterpret_cast<GUEST_PTR *>(static_cast<uintptr_t>(block));
	auto *text = reinterpret_cast<uint16_t *>(argv + arguments.size() + 1);
	for (size_t i = 0; i < arguments.size(); ++i) {
		argv[i] = toGuestPtr(text);
		std::memcpy(text, arguments[i].c_str(), (arguments[i].size() + 1) * sizeof(uint16_t));
		text += arguments[i].size() + 1;
	}
	argv[arguments.size()] = GUEST_NULL;
	*pNumArgs = static_cast<int>(arguments.size());
	return argv;
}

} // namespace shell32

#include "shell32_trampolines.h"

extern const wibo::ModuleStub lib_shell32 = {
	(const char *[]){"shell32", nullptr},
	shell32ThunkByName,
	nullptr,
};
