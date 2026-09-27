#include "user32.h"
#include "user32/internal.h"

#include <atomic>

#include "common.h"
#include "context.h"
#include "errors.h"
#include "kernel32/internal.h"
#include "modules.h"
#include "resources.h"
#include "strutil.h"
#include "system_provider.h"

#include <array>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

std::atomic<bool> g_ghostingDisabled = false;
std::mutex g_atomMutex;
std::unordered_map<std::u16string, UINT> g_registeredAtoms;
constexpr size_t kAlphaTableBytes = 0x10000 / 8;
std::mutex g_alphaTableMutex;
std::array<uint8_t, kAlphaTableBytes> g_alphaTable{};
bool g_alphaTableLoaded = false;

bool classifyAlphabetic(WCHAR character, BOOL &result) {
	std::lock_guard lock(g_alphaTableMutex);
	if (!g_alphaTableLoaded) {
		std::vector<uint8_t> response;
		if (!wibo::provider::request({"is-char-alpha-w-table"}, response)) {
			kernel32::setLastError(ERROR_NOT_SUPPORTED);
			return false;
		}
		wibo::provider::Reader reader(response);
		int32_t status;
		if (!reader.header(status)) {
			kernel32::setLastError(ERROR_INVALID_DATA);
			return false;
		}
		if (status) {
			if (!reader.done())
				kernel32::setLastError(ERROR_INVALID_DATA);
			else
				kernel32::setLastError(status == wibo::provider::kUnavailable ? ERROR_NOT_SUPPORTED
																			  : static_cast<DWORD>(status));
			return false;
		}
		std::vector<uint8_t> table;
		if (!reader.bytes(table) || !reader.done() || table.size() != g_alphaTable.size()) {
			kernel32::setLastError(ERROR_INVALID_DATA);
			return false;
		}
		std::memcpy(g_alphaTable.data(), table.data(), table.size());
		g_alphaTableLoaded = true;
	}
	result = (g_alphaTable[character / 8] & (1u << (character % 8))) != 0;
	return true;
}

UINT registerAtom(LPCWSTR lpString) {
	if (!lpString || !*lpString) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	const size_t length = wstrnlen(lpString, 256);
	if (length > 255) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return 0;
	}
	std::u16string name;
	for (size_t i = 0; i < length; ++i) {
		name.push_back(static_cast<char16_t>(wcharToLower(lpString[i])));
	}
	std::lock_guard lock(g_atomMutex);
	if (auto it = g_registeredAtoms.find(name); it != g_registeredAtoms.end()) {
		return it->second;
	}
	if (g_registeredAtoms.size() == 0x4000) {
		kernel32::setLastError(ERROR_NOT_ENOUGH_MEMORY);
		return 0;
	}
	// Registration is local to the process until interprocess data exchange is supported.
	const UINT message = 0xC000 + static_cast<UINT>(g_registeredAtoms.size());
	g_registeredAtoms.emplace(std::move(name), message);
	return message;
}

} // namespace

namespace user32::detail {
bool ghostingDisabled() { return g_ghostingDisabled.load(); }
} // namespace user32::detail

namespace user32 {

BOOL WINAPI IsCharAlphaW(WCHAR character) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("IsCharAlphaW(0x%04x)\n", static_cast<unsigned int>(character));
	const DWORD lastError = kernel32::getLastError();
	BOOL result;
	if (character < 0x80) {
		result = (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
	} else if (!classifyAlphabetic(character, result)) {
		return FALSE;
	}
	kernel32::setLastError(lastError);
	return result;
}

void WINAPI DisableProcessWindowsGhosting() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("DisableProcessWindowsGhosting()\n");
	g_ghostingDisabled.store(true);
}

UINT WINAPI RegisterWindowMessageW(LPCWSTR lpString) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegisterWindowMessageW(%p)\n", lpString);
	return registerAtom(lpString);
}

UINT WINAPI RegisterClipboardFormatW(LPCWSTR name) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegisterClipboardFormatW(%p)\n", name);
	return registerAtom(name);
}

UINT WINAPI RegisterClipboardFormatA(LPCSTR name) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegisterClipboardFormatA(%p)\n", name);
	const auto wide = stringToWideString(name);
	return registerAtom(name ? wide.data() : nullptr);
}

UINT WINAPI RegisterWindowMessageA(LPCSTR lpString) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RegisterWindowMessageA(%s)\n", lpString);
	const auto name = stringToWideString(lpString);
	return RegisterWindowMessageW(lpString ? name.data() : nullptr);
}

constexpr uint32_t RT_STRING_ID = 6;
constexpr HKL kDefaultKeyboardLayout = 0x04090409;
constexpr int UOI_FLAGS = 1;

struct USEROBJECTFLAGS {
	BOOL fInherit;
	BOOL fReserved;
	DWORD dwFlags;
};

int WINAPI LoadStringA(HMODULE hInstance, UINT uID, LPSTR lpBuffer, int cchBufferMax) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("LoadStringA(%p, %u, %p, %d)\n", hInstance, uID, lpBuffer, cchBufferMax);
	if (!lpBuffer || cchBufferMax <= 0) {
		return 0;
	}
	wibo::Executable *mod = wibo::executableFromModule((HMODULE)hInstance);
	if (!mod) {
		return 0;
	}
	wibo::ResourceIdentifier type = wibo::ResourceIdentifier::fromID(RT_STRING_ID);
	wibo::ResourceIdentifier table = wibo::ResourceIdentifier::fromID((uID >> 4) + 1);
	wibo::ResourceLocation loc;
	if (!mod->findResource(type, table, std::nullopt, loc)) {
		return 0;
	}
	const uint16_t *cursor = reinterpret_cast<const uint16_t *>(loc.data);
	const uint16_t *end = cursor + (loc.size / sizeof(uint16_t));
	unsigned int entryIndex = uID & 0x0Fu;
	for (unsigned int i = 0; i < entryIndex; ++i) {
		if (cursor >= end) {
			return 0;
		}
		uint16_t length = *cursor++;
		if (cursor + length > end) {
			return 0;
		}
		cursor += length;
	}
	if (cursor >= end) {
		return 0;
	}
	uint16_t length = *cursor++;
	if (cursor + length > end) {
		return 0;
	}
	int copyLength = length;
	if (copyLength > cchBufferMax - 1) {
		copyLength = cchBufferMax - 1;
	}
	for (int i = 0; i < copyLength; ++i) {
		lpBuffer[i] = static_cast<char>(cursor[i] & 0xFF);
	}
	lpBuffer[copyLength] = 0;
	DEBUG_LOG("LoadStringA -> %.*s\n", copyLength, lpBuffer);
	return copyLength;
}

int WINAPI LoadStringW(HMODULE hInstance, UINT uID, LPWSTR lpBuffer, int cchBufferMax) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("LoadStringW(%p, %u, %p, %d)\n", hInstance, uID, lpBuffer, cchBufferMax);
	wibo::Executable *mod = wibo::executableFromModule((HMODULE)hInstance);
	if (!mod) {
		return 0;
	}
	wibo::ResourceIdentifier type = wibo::ResourceIdentifier::fromID(RT_STRING_ID);
	wibo::ResourceIdentifier table = wibo::ResourceIdentifier::fromID((uID >> 4) + 1);
	wibo::ResourceLocation loc;
	if (!mod->findResource(type, table, std::nullopt, loc)) {
		return 0;
	}
	const uint16_t *cursor = reinterpret_cast<const uint16_t *>(loc.data);
	const uint16_t *end = cursor + (loc.size / sizeof(uint16_t));
	unsigned int entryIndex = uID & 0x0Fu;
	for (unsigned int i = 0; i < entryIndex; ++i) {
		if (cursor >= end) {
			return 0;
		}
		uint16_t length = *cursor++;
		if (cursor + length > end) {
			return 0;
		}
		cursor += length;
	}
	if (cursor >= end) {
		return 0;
	}
	uint16_t length = *cursor++;
	if (cursor + length > end) {
		return 0;
	}
	if (cchBufferMax == 0) {
		if (lpBuffer) {
			*reinterpret_cast<uint16_t **>(lpBuffer) = const_cast<uint16_t *>(cursor);
		}
		return length;
	}
	if (!lpBuffer || cchBufferMax <= 0) {
		return 0;
	}
	int copyLength = length;
	if (copyLength > cchBufferMax - 1) {
		copyLength = cchBufferMax - 1;
	}
	for (int i = 0; i < copyLength; ++i) {
		lpBuffer[i] = cursor[i];
	}
	lpBuffer[copyLength] = 0;
	DEBUG_LOG("LoadStringW -> length %d\n", copyLength);
	return copyLength;
}

int WINAPI MessageBoxA(HWND hwnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType) {
	HOST_CONTEXT_GUARD();
	(void)hwnd;
	(void)uType;
	printf("MESSAGE BOX: [%s] %s\n", lpCaption, lpText);
	fflush(stdout);
	return 1;
}

HKL WINAPI GetKeyboardLayout(DWORD idThread) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: GetKeyboardLayout(%u)\n", idThread);
	(void)idThread;
	return kDefaultKeyboardLayout;
}

HWINSTA WINAPI GetProcessWindowStation() {
	DEBUG_LOG("STUB: GetProcessWindowStation()\n");
	return NO_HANDLE;
}

HANDLE WINAPI GetThreadDesktop(DWORD dwThreadId) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetThreadDesktop(%u)\n", dwThreadId);
	// No desktop is assigned to guest threads in this console runtime.
	kernel32::setLastError(ERROR_NOT_SUPPORTED);
	return NO_HANDLE;
}

BOOL WINAPI GetUserObjectInformationA(HANDLE hObj, int nIndex, PVOID pvInfo, DWORD nLength, LPDWORD lpnLengthNeeded) {
	DEBUG_LOG("GetUserObjectInformationA(%p, %d, %p, %u, %p)\n", hObj, nIndex, pvInfo, nLength, lpnLengthNeeded);
	(void)hObj;

	if (lpnLengthNeeded) {
		*lpnLengthNeeded = sizeof(USEROBJECTFLAGS);
	}

	if (nIndex != UOI_FLAGS) {
		kernel32::setLastError(ERROR_CALL_NOT_IMPLEMENTED);
		return FALSE;
	}

	if (!pvInfo || nLength < sizeof(USEROBJECTFLAGS)) {
		kernel32::setLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}

	auto *flags = reinterpret_cast<USEROBJECTFLAGS *>(pvInfo);
	flags->fInherit = FALSE;
	flags->fReserved = FALSE;
	// Wibo has no desktop or interactive window station. Reporting visibility
	// here makes console tools take GUI-only error paths and call into GDI even
	// though GetProcessWindowStation returned no handle.
	flags->dwFlags = 0;
	return TRUE;
}

HWND WINAPI GetActiveWindow() {
	DEBUG_LOG("GetActiveWindow()\n");
	return NO_HANDLE;
}

DWORD WINAPI GetSysColor(int nIndex) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("GetSysColor(%d)\n", nIndex);
	// Stable classic Windows colors are preferable to querying host UI state:
	// wibo is also used on headless Linux hosts where no desktop theme exists.
	switch (nIndex) {
	case 5: // COLOR_WINDOW
		return 0x00FFFFFF;
	case 13: // COLOR_HIGHLIGHT
		return 0x00D77800;
	case 15: // COLOR_3DFACE / COLOR_BTNFACE
		return 0x00C0C0C0;
	case 17: // COLOR_GRAYTEXT
		return 0x00808080;
	default:
		return nIndex >= 0 && nIndex <= 30 ? 0x00000000 : 0;
	}
}

} // namespace user32

#include "user32_trampolines.h"

extern const wibo::ModuleStub lib_user32 = {
	(const char *[]){
		"user32",
		nullptr,
	},
	user32ThunkByName,
	nullptr,
};
