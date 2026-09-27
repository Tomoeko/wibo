#include "shell32.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "files.h"
#include "kernel32/fileapi.h"
#include "kernel32/handleapi.h"
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

#include <array>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>
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

class ExecutableFile {
  public:
	explicit ExecutableFile(HANDLE handle)
		: handle_(handle), file_(wibo::handles().getAs<kernel32::FileObject>(handle)) {
		if (!file_) {
			kernel32::setLastError(ERROR_INVALID_HANDLE);
			return;
		}
		const int descriptor = fcntl(file_->fd, F_DUPFD_CLOEXEC, 0);
		if (descriptor < 0) {
			kernel32::setLastError(wibo::winErrorFromErrno(errno));
			return;
		}
		stream_.reset(fdopen(descriptor, "rb"));
		if (!stream_) {
			const int error = errno;
			close(descriptor);
			kernel32::setLastError(wibo::winErrorFromErrno(error));
		}
	}
	~ExecutableFile() {
		const DWORD error = kernel32::getLastError();
		stream_.reset();
		kernel32::CloseHandle(handle_);
		kernel32::setLastError(error);
	}
	[[nodiscard]] FILE *stream() const { return stream_.get(); }
	template <size_t Size> bool readFile(uint64_t offset, std::array<BYTE, Size> &header) const {
		if (offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) - Size) {
			kernel32::setLastError(ERROR_NOT_SUPPORTED);
			return false;
		}
		const auto result = files::read(file_.get(), header.data(), Size, static_cast<off_t>(offset), false);
		if (result.windowsError || result.unixError) {
			kernel32::setLastError(result.windowsError ? result.windowsError
													   : wibo::winErrorFromErrno(result.unixError));
			return false;
		}
		return result.bytesTransferred == Size;
	}

	// Image classification has mapped-file semantics, which do not observe byte-range locks.
	template <size_t Size> bool readImage(uint64_t offset, std::array<BYTE, Size> &header) const {
		size_t read = 0;
		while (read < Size) {
			if (offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) - read)
				return false;
			const ssize_t count =
				pread(fileno(stream_.get()), header.data() + read, Size - read, static_cast<off_t>(offset + read));
			if (count < 0 && errno == EINTR)
				continue;
			if (count < 0)
				kernel32::setLastError(wibo::winErrorFromErrno(errno));
			if (count <= 0)
				return false;
			read += static_cast<size_t>(count);
		}
		return true;
	}

  private:
	HANDLE handle_;
	Pin<kernel32::FileObject> file_;
	std::unique_ptr<FILE, decltype(&fclose)> stream_{nullptr, fclose};
};

WORD headerWord(const BYTE *bytes) { return static_cast<WORD>(bytes[0] | (static_cast<WORD>(bytes[1]) << 8)); }

DWORD headerDword(const BYTE *bytes) {
	return static_cast<DWORD>(headerWord(bytes)) | (static_cast<DWORD>(headerWord(bytes + 2)) << 16);
}

DWORD executableType(LPCWSTR path, const std::filesystem::path &hostPath) {
	const HANDLE handle = kernel32::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, 0);
	if (handle == INVALID_HANDLE_VALUE) {
		if (kernel32::getLastError() == ERROR_FILE_NOT_FOUND)
			kernel32::setLastError(missingPathError(hostPath));
		return 0;
	}
	kernel32::setLastError(ERROR_SUCCESS);
	ExecutableFile file(handle);
	if (!file.stream())
		return 0;
	std::array<BYTE, 64> dos{};
	if (!file.readImage(0, dos))
		return 0;
	if (headerWord(dos.data()) != 0x5A4D) {
		std::string extension = hostPath.extension().string();
		toLowerInPlace(extension);
		return extension == ".com" || extension == ".pif" ? 0x4D5A : 0;
	}
	const DWORD offset = headerDword(dos.data() + 0x3C);
	std::array<BYTE, 24> coff{};
	if (!file.readImage(offset, coff))
		return kernel32::getLastError() ? 0 : 0x4D5A;
	if (headerWord(coff.data()) == 0x454E) {
		std::array<BYTE, 64> ne{};
		std::array<BYTE, 4> signature{};
		if (!file.readFile(0, dos) || !file.readFile(offset, signature) || !file.readFile(offset, ne))
			return 0;
		return ne[0x36] == 2 ? 0x454E | (static_cast<DWORD>(headerWord(ne.data() + 0x3E)) << 16) : 0;
	}
	if (headerDword(coff.data()) != 0x4550)
		return 0x4D5A;
	const WORD machine = headerWord(coff.data() + 4);
	if (machine != 0x14C && machine != 0x8664) {
		if (machine == 0x1C4 || machine == 0xAA64)
			kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return 0;
	}
	const DWORD openingError = kernel32::getLastError();
	size_t imageSize = 0;
	if (!wibo::Executable::imageMappingSize(file.stream(), imageSize)) {
		if (kernel32::getLastError() != ERROR_NOT_SUPPORTED)
			kernel32::setLastError(openingError);
		return 0;
	}
	kernel32::setLastError(openingError);
	if (headerWord(coff.data() + 22) & 0x2000)
		return 0;
	std::array<BYTE, 4> signature{};
#ifdef WIBO_GUEST_64
	std::array<BYTE, 264> nt{};
#else
	std::array<BYTE, 248> nt{};
#endif
	if (!file.readFile(0, dos) || !file.readFile(offset, signature) || !file.readFile(offset, nt))
		return 0;
	const BYTE *optional = nt.data() + coff.size();
	DWORD result = 0x4550;
	if (headerWord(optional + 68) == 2) {
		result |= static_cast<DWORD>(headerWord(optional + 48)) << 24;
		result |= static_cast<DWORD>(headerWord(optional + 50)) << 16;
	}
	return result;
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

DWORD_PTR WINAPI SHGetFileInfoW(LPCWSTR path, DWORD attributes, SHFILEINFOW *info, UINT size, UINT flags) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SHGetFileInfoW(%p, %u, %p, %u, 0x%x)\n", path, attributes, info, size, flags);
	if (!path)
		return 0;
	if (info) {
		info->szDisplayName[0] = 0;
		info->szTypeName[0] = 0;
		info->iIcon = 0;
	}
	constexpr UINT executableTypeFlag = 0x2000;
	if (flags & executableTypeFlag) {
		if (flags != executableTypeFlag)
			return 0;
		const size_t length = wstrnlen(path, MAX_PATH);
		std::string name;
		if (length == MAX_PATH ||
			!utf16ToUtf8(std::u16string_view(reinterpret_cast<const char16_t *>(path), length), name) ||
			!supportsShellPath(name)) {
			kernel32::setLastError(ERROR_NOT_SUPPORTED);
			return 0;
		}
		if (name.empty()) {
			kernel32::setLastError(ERROR_ACCESS_DENIED);
			return 0;
		}
		auto hostPath = files::pathFromWindows(name.c_str());
		if (hostPath.is_relative()) {
			std::error_code error;
			const auto current = std::filesystem::current_path(error);
			if (error) {
				kernel32::setLastError(wibo::winErrorFromErrno(error.value()));
				return 0;
			}
			hostPath = current / hostPath;
		}
		return executableType(path, hostPath);
	}
	// Display names, attributes, associations, and icon handles require an owned shell context.
	kernel32::setLastError(ERROR_NOT_SUPPORTED);
	return 0;
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
