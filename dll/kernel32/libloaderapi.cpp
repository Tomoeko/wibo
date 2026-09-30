#include "libloaderapi.h"
#include "processthreadsapi.h"

#include "context.h"
#include "errors.h"
#include "files.h"
#include "modules.h"
#include "psapi.h"
#include "resources.h"
#include "strutil.h"
#include "types.h"

#include <algorithm>
#include <cassert>
#include <climits>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr DWORD GET_MODULE_HANDLE_EX_FLAG_PIN = 0x00000001;
constexpr DWORD GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT = 0x00000002;
constexpr DWORD GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS = 0x00000004;
constexpr DWORD GET_MODULE_HANDLE_EX_VALID_FLAGS = GET_MODULE_HANDLE_EX_FLAG_PIN |
												   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT |
												   GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS;
constexpr DWORD kLoadAsDatafile = 0x00000002;
constexpr DWORD kLoadAsImageResource = 0x00000020;
constexpr DWORD kLoadAsDatafileExclusive = 0x00000040;
constexpr DWORD kResourceLoadModes = kLoadAsDatafile | kLoadAsImageResource | kLoadAsDatafileExclusive;

HRSRC findResourceInternal(HMODULE hModule, const wibo::ResourceIdentifier &type, const wibo::ResourceIdentifier &name,
						   std::optional<uint16_t> language) {
	auto *exe = wibo::executableFromModule(hModule);
	if (!exe) {
		kernel32::setLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
		return NO_HANDLE;
	}
	wibo::ResourceLocation loc;
	if (!exe->findResource(type, name, language, loc)) {
		return NO_HANDLE;
	}
	return toGuestPtr(loc.dataEntry);
}

} // namespace

namespace kernel32 {

BOOL WINAPI DisableThreadLibraryCalls(HMODULE hLibModule) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("DisableThreadLibraryCalls(%p)\n", hLibModule);
	if (!hLibModule) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (!wibo::disableThreadNotifications(hLibModule)) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	return TRUE;
}

HMODULE WINAPI GetModuleHandleA(LPCSTR lpModuleName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetModuleHandleA(%s)\n", lpModuleName);
	const auto *module = wibo::findLoadedModule(lpModuleName);
	if (!module) {
		setLastError(ERROR_MOD_NOT_FOUND);
		return NO_HANDLE;
	}
	return module->handle;
}

HMODULE WINAPI GetModuleHandleW(LPCWSTR lpModuleName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetModuleHandleW -> ");
	if (lpModuleName) {
		const auto lpModuleNameA = wideStringToString(lpModuleName);
		return GetModuleHandleA(lpModuleNameA.c_str());
	}
	return GetModuleHandleA(nullptr);
}

BOOL WINAPI GetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE *phModule) {
	HOST_CONTEXT_GUARD();
	if (!lpModuleName || (dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS)) {
		return GetModuleHandleExA(dwFlags, reinterpret_cast<LPCSTR>(lpModuleName), phModule);
	}
	const auto name = wideStringToString(lpModuleName);
	return GetModuleHandleExA(dwFlags, name.c_str(), phModule);
}

BOOL WINAPI GetModuleHandleExA(DWORD dwFlags, LPCSTR lpModuleName, HMODULE *phModule) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetModuleHandleExA(%x, %p, %p)\n", dwFlags, lpModuleName, phModule);
	if (!phModule) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*phModule = NO_HANDLE;
	if ((dwFlags & ~GET_MODULE_HANDLE_EX_VALID_FLAGS) != 0 ||
		((dwFlags & GET_MODULE_HANDLE_EX_FLAG_PIN) && (dwFlags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT))) {
		setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	const HMODULE module = wibo::acquireModuleHandle(lpModuleName, dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
													 dwFlags & GET_MODULE_HANDLE_EX_FLAG_PIN,
													 dwFlags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT);
	if (!module) {
		setLastError(ERROR_MOD_NOT_FOUND);
		return FALSE;
	}
	*phModule = module;
	return TRUE;
}

DWORD WINAPI GetModuleFileNameA(HMODULE hModule, LPSTR lpFilename, DWORD nSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetModuleFileNameA(%p, %p, %u)\n", hModule, lpFilename, nSize);
	if (!lpFilename) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	auto *info = wibo::moduleInfoFromHandle(hModule);
	if (!info) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	std::string path;
	if (!info->resolvedPath.empty()) {
		path = files::pathToWindows(info->resolvedPath);
	} else {
		path = info->originalName;
	}
	DEBUG_LOG("-> %s\n", path.c_str());
	if (nSize == 0) {
		setLastError(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}
	const size_t len = path.size();
	const size_t copyLen = std::min(len, static_cast<size_t>(nSize - 1));
	std::memcpy(lpFilename, path.c_str(), copyLen);
	if (copyLen < nSize) {
		lpFilename[copyLen] = '\0';
	}
	if (copyLen < len) {
		setLastError(ERROR_INSUFFICIENT_BUFFER);
		return nSize;
	}
	return static_cast<DWORD>(copyLen);
}

DWORD WINAPI GetModuleFileNameW(HMODULE hModule, LPWSTR lpFilename, DWORD nSize) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetModuleFileNameW(%p, %p, %u)\n", hModule, lpFilename, nSize);
	if (!lpFilename) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	auto *info = wibo::moduleInfoFromHandle(hModule);
	if (!info) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	std::string path;
	if (!info->resolvedPath.empty()) {
		path = files::pathToWindows(info->resolvedPath);
	} else {
		path = info->originalName;
	}
	if (nSize == 0) {
		setLastError(ERROR_INSUFFICIENT_BUFFER);
		return 0;
	}
	auto wide = stringToWideString(path.c_str());
	if (wide.empty()) {
		wide.push_back(0);
	}
	const size_t len = wide.size() - 1;
	const size_t copyLen = std::min(len, static_cast<size_t>(nSize - 1));
	for (size_t i = 0; i < copyLen; ++i) {
		lpFilename[i] = wide[i];
	}
	if (copyLen < static_cast<size_t>(nSize)) {
		lpFilename[copyLen] = 0;
	}
	if (copyLen < len) {
		setLastError(ERROR_INSUFFICIENT_BUFFER);
		return nSize;
	}
	return static_cast<DWORD>(copyLen);
}

HRSRC WINAPI FindResourceA(HMODULE hModule, LPCSTR lpName, LPCSTR lpType) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FindResourceA(%p, %p, %p)\n", hModule, lpName, lpType);
	auto type = wibo::resourceIdentifierFromAnsi(lpType);
	auto name = wibo::resourceIdentifierFromAnsi(lpName);
	return findResourceInternal(hModule, type, name, std::nullopt);
}

HRSRC WINAPI FindResourceExA(HMODULE hModule, LPCSTR lpType, LPCSTR lpName, WORD wLanguage) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FindResourceExA(%p, %p, %p, %u)\n", hModule, lpName, lpType, wLanguage);
	auto type = wibo::resourceIdentifierFromAnsi(lpType);
	auto name = wibo::resourceIdentifierFromAnsi(lpName);
	return findResourceInternal(hModule, type, name, wLanguage);
}

HRSRC WINAPI FindResourceW(HMODULE hModule, LPCWSTR lpName, LPCWSTR lpType) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FindResourceW(%p, %p, %p)\n", hModule, lpName, lpType);
	auto type = wibo::resourceIdentifierFromWide(lpType);
	auto name = wibo::resourceIdentifierFromWide(lpName);
	return findResourceInternal(hModule, type, name, std::nullopt);
}

HRSRC WINAPI FindResourceExW(HMODULE hModule, LPCWSTR lpType, LPCWSTR lpName, WORD wLanguage) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FindResourceExW(%p, %p, %p, %u)\n", hModule, lpName, lpType, wLanguage);
	auto type = wibo::resourceIdentifierFromWide(lpType);
	auto name = wibo::resourceIdentifierFromWide(lpName);
	return findResourceInternal(hModule, type, name, wLanguage);
}

HGLOBAL WINAPI LoadResource(HMODULE hModule, HRSRC hResInfo) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("LoadResource(%p, %p)\n", hModule, hResInfo);
	if (!hResInfo) {
		setLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
		return GUEST_NULL;
	}
	auto *exe = wibo::executableFromModule(hModule);
	if (!exe || !exe->rsrcBase) {
		setLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
		return GUEST_NULL;
	}
	const auto *entry = reinterpret_cast<const wibo::ImageResourceDataEntry *>(hResInfo);
	if (!wibo::resourceEntryBelongsToExecutable(*exe, entry)) {
		setLastError(ERROR_INVALID_PARAMETER);
		return GUEST_NULL;
	}
	return toGuestPtr(exe->fromRVA<const void>(entry->offsetToData));
}

BOOL WINAPI FreeResource(HGLOBAL hResData) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FreeResource(%p)\n", fromGuestPtr(hResData));
	// Win32 resources remain mapped with their module. The legacy release
	// function always returns FALSE and does not release LoadResource bytes.
	return FALSE;
}

LPVOID WINAPI LockResource(HGLOBAL hResData) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: LockResource(%p)\n", hResData);
	return fromGuestPtr(hResData);
}

DWORD WINAPI SizeofResource(HMODULE hModule, HRSRC hResInfo) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("SizeofResource(%p, %p)\n", hModule, hResInfo);
	if (!hResInfo) {
		setLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
		return 0;
	}
	auto *exe = wibo::executableFromModule(hModule);
	if (!exe || !exe->rsrcBase) {
		setLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
		return 0;
	}
	const auto *entry = reinterpret_cast<const wibo::ImageResourceDataEntry *>(hResInfo);
	if (!wibo::resourceEntryBelongsToExecutable(*exe, entry)) {
		setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	return entry->size;
}

HMODULE WINAPI LoadLibraryA(LPCSTR lpLibFileName) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("LoadLibraryA(%s)\n", lpLibFileName);
	const auto *info = wibo::loadModule(lpLibFileName);
	if (!info) {
		// lastError is set by loadModule
		return NO_HANDLE;
	}
	return info->handle;
}

HMODULE WINAPI LoadLibraryW(LPCWSTR lpLibFileName) {
	HOST_CONTEXT_GUARD();
	if (!lpLibFileName) {
		return NO_HANDLE;
	}
	auto filename = wideStringToString(lpLibFileName);
	DEBUG_LOG("LoadLibraryW(%s)\n", filename.c_str());
	return LoadLibraryA(filename.c_str());
}

HMODULE WINAPI LoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("LoadLibraryExA(%s, %llx, %x)\n", lpLibFileName ? lpLibFileName : "", static_cast<ULONGLONG>(hFile),
			  dwFlags);
	// The reserved handle does not select a file or change the load mode.
	(void)hFile;
	constexpr DWORD searchModes = wibo::ModuleSearch::AlteredPath | wibo::ModuleSearch::DirectoryMask;
	if (dwFlags & ~(searchModes | kResourceLoadModes)) {
		DEBUG_LOG("LoadLibraryExA: unsupported load mode\n");
		setLastError(ERROR_NOT_SUPPORTED);
		return NO_HANDLE;
	}
	if ((dwFlags & kLoadAsDatafile) && (dwFlags & kLoadAsDatafileExclusive)) {
		setLastError(ERROR_INVALID_PARAMETER);
		return NO_HANDLE;
	}
	if (!lpLibFileName) {
		setLastError(ERROR_MOD_NOT_FOUND);
		return NO_HANDLE;
	}
	std::string filename = lpLibFileName;
	while (!filename.empty() && filename.back() == ' ')
		filename.pop_back();
	const size_t lastSeparator = filename.find_last_of("\\/");
	const size_t baseStart = lastSeparator == std::string::npos ? 0 : lastSeparator + 1;
	const size_t lastCharacter = filename.find_last_not_of('.');
	if (lastCharacter != std::string::npos && filename.find('.', baseStart) < lastCharacter)
		filename.resize(lastCharacter + 1);
	if (filename.empty()) {
		setLastError(ERROR_MOD_NOT_FOUND);
		return NO_HANDLE;
	}
	if (dwFlags & kResourceLoadModes)
		return wibo::loadResourceModule(filename.c_str(), dwFlags);
	const auto *info = wibo::loadModule(filename.c_str(), dwFlags);
	return info ? info->handle : NO_HANDLE;
}

HMODULE WINAPI LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("LoadLibraryExW(%x) -> ", dwFlags);
	if (!lpLibFileName)
		return LoadLibraryExA(nullptr, hFile, dwFlags);
	std::string filename;
	if (!utf16ToUtf8(std::u16string_view(reinterpret_cast<const char16_t *>(lpLibFileName), wstrlen(lpLibFileName)),
					 filename)) {
		setLastError(ERROR_INVALID_PARAMETER);
		return NO_HANDLE;
	}
	return LoadLibraryExA(filename.c_str(), hFile, dwFlags);
}

[[noreturn]] VOID WINAPI FreeLibraryAndExitThread(HMODULE module, DWORD exitCode) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FreeLibraryAndExitThread(%p, %u)\n", module, exitCode);
	// The return path stays in the host shim after releasing the guest module.
	FreeLibrary(module);
	ExitThread(exitCode);
}

BOOL WINAPI FreeLibrary(HMODULE hLibModule) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("FreeLibrary(%p)\n", hLibModule);
	if (wibo::freeResourceModule(hLibModule))
		return TRUE;
	auto *info = wibo::moduleInfoFromHandle(hLibModule);
	if (!info) {
		setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	wibo::freeModule(info);
	return TRUE;
}

FARPROC WINAPI GetProcAddress(HMODULE hModule, LPCSTR lpProcName) {
	HOST_CONTEXT_GUARD();
	FARPROC result;
	const auto info = wibo::moduleInfoFromHandle(hModule);
	if (!info) {
		DEBUG_LOG("GetProcAddress(%p) -> ERROR_INVALID_HANDLE\n", hModule);
		setLastError(ERROR_INVALID_HANDLE);
		return nullptr;
	}
	const auto proc = reinterpret_cast<uintptr_t>(lpProcName);
	if (proc & ~uintptr_t{0xFFFF}) {
		DEBUG_LOG("GetProcAddress(%s, %s) ", info->normalizedName.c_str(), lpProcName);
		result = wibo::findExportByName(info, lpProcName);
	} else {
		DEBUG_LOG("GetProcAddress(%s, %u) ", info->normalizedName.c_str(), proc);
		result = wibo::findExportByOrdinal(info, static_cast<uint16_t>(proc));
	}
	DEBUG_LOG("-> %p\n", result);
	if (!result) {
		setLastError(ERROR_PROC_NOT_FOUND);
	}
	return result;
}

BOOL WINAPI K32EnumProcessModules(HANDLE process, HMODULE *modules, DWORD capacity, LPDWORD needed) {
	return psapi::EnumProcessModules(process, modules, capacity, needed);
}

DWORD WINAPI K32GetModuleBaseNameA(HANDLE process, HMODULE module, LPSTR name, DWORD size) {
	return psapi::GetModuleBaseNameA(process, module, name, size);
}

DWORD WINAPI K32GetModuleBaseNameW(HANDLE process, HMODULE module, LPWSTR name, DWORD size) {
	return psapi::GetModuleBaseNameW(process, module, name, size);
}

DWORD WINAPI K32GetModuleFileNameExA(HANDLE process, HMODULE module, LPSTR filename, DWORD size) {
	return psapi::GetModuleFileNameExA(process, module, filename, size);
}

DWORD WINAPI K32GetModuleFileNameExW(HANDLE process, HMODULE module, LPWSTR filename, DWORD size) {
	return psapi::GetModuleFileNameExW(process, module, filename, size);
}

BOOL WINAPI K32GetModuleInformation(HANDLE process, HMODULE module, LPMODULEINFO information, DWORD size) {
	return psapi::GetModuleInformation(process, module, information, size);
}

BOOL WINAPI K32GetProcessMemoryInfo(HANDLE process, PPROCESS_MEMORY_COUNTERS counters, DWORD size) {
	return psapi::GetProcessMemoryInfo(process, counters, size);
}

} // namespace kernel32
