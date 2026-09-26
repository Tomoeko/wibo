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
#include "modules.h"
#include "strutil.h"

#include <cstring>
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
