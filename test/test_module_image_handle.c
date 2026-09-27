#define _WIN32_WINNT 0x0501
#define PSAPI_VERSION 1
// clang-format off: MinGW's psapi.h requires the Windows base types first.
#include <windows.h>
#include <psapi.h>
// clang-format on

#include "test_assert.h"

static void checkImageHandle(HMODULE module) {
	MODULEINFO info;
	TEST_CHECK(GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)));
	// Verify the handle before dereferencing it so a broken loader fails safely.
	TEST_CHECK(info.lpBaseOfDll == (LPVOID)module);
	TEST_CHECK(info.SizeOfImage >= sizeof(IMAGE_DOS_HEADER) + sizeof(IMAGE_NT_HEADERS));

	MEMORY_BASIC_INFORMATION memory;
	TEST_CHECK_EQ(sizeof(memory), VirtualQuery(module, &memory, sizeof(memory)));
	TEST_CHECK(memory.AllocationBase == (LPVOID)module);
	TEST_CHECK_EQ(MEM_COMMIT, memory.State);
	TEST_CHECK_EQ(MEM_IMAGE, memory.Type);
	TEST_CHECK(memory.Protect != PAGE_NOACCESS);

	const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)module;
	TEST_CHECK_EQ(IMAGE_DOS_SIGNATURE, dos->e_magic);
	TEST_CHECK(dos->e_lfanew >= (LONG)sizeof(*dos));
	TEST_CHECK((DWORD)dos->e_lfanew <= info.SizeOfImage - sizeof(IMAGE_NT_HEADERS));
	const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const BYTE *)module + dos->e_lfanew);
	TEST_CHECK_EQ(IMAGE_NT_SIGNATURE, nt->Signature);
	TEST_CHECK_EQ(info.SizeOfImage, nt->OptionalHeader.SizeOfImage);
	TEST_CHECK(info.EntryPoint == (const BYTE *)module + nt->OptionalHeader.AddressOfEntryPoint);
}

static void checkExternalModule(DWORD flags) {
	HMODULE module = LoadLibraryExW(L"external_exports.dll", NULL, flags);
	TEST_CHECK_MSG(module != NULL, "LoadLibraryExW failed: %lu", (unsigned long)GetLastError());
	checkImageHandle(module);
	TEST_CHECK(GetModuleHandleA("EXTERNAL_EXPORTS.DLL") == module);
	TEST_CHECK(GetModuleHandleW(L"external_exports") == module);

	WCHAR filename[MAX_PATH];
	DWORD length = GetModuleFileNameW(module, filename, MAX_PATH);
	TEST_CHECK(length != 0 && length < MAX_PATH);
	HMODULE byPath = LoadLibraryW(filename);
	TEST_CHECK(byPath == module);
	HMODULE byName = LoadLibraryA("EXTERNAL_EXPORTS.DLL");
	TEST_CHECK(byName == module);

#ifdef _WIN64
	const char *addName = "add_numbers";
#else
	const char *addName = "add_numbers@8";
#endif
	FARPROC address = GetProcAddress(module, addName);
	TEST_CHECK(address != NULL);
	MODULEINFO info;
	TEST_CHECK(GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)));
	TEST_CHECK((ULONG_PTR)address >= (ULONG_PTR)module);
	TEST_CHECK((ULONG_PTR)address - (ULONG_PTR)module < info.SizeOfImage);

	HMODULE byAddress = NULL;
	TEST_CHECK(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
								  (LPCSTR)address, &byAddress));
	TEST_CHECK(byAddress == module);
	TEST_CHECK(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)address, &byAddress));
	TEST_CHECK(byAddress == module);

	HMODULE modules[128];
	DWORD needed = 0;
	TEST_CHECK(EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed));
	TEST_CHECK(needed <= sizeof(modules));
	BOOL found = FALSE;
	for (DWORD i = 0; i < needed / sizeof(HMODULE); ++i) {
		if (modules[i] == module)
			found = TRUE;
	}
	TEST_CHECK(found);

	TEST_CHECK(FreeLibrary(byAddress));
	TEST_CHECK(FreeLibrary(byPath));
	TEST_CHECK(FreeLibrary(byName));
	TEST_CHECK(GetModuleHandleW(L"external_exports.dll") == module);
	checkImageHandle(module);
	TEST_CHECK(FreeLibrary(module));
	TEST_CHECK(GetModuleHandleW(L"external_exports.dll") == NULL);
}

int main(void) {
	checkImageHandle(GetModuleHandleW(NULL));
	for (unsigned int i = 0; i < 4; ++i)
		checkExternalModule(i % 2 ? LOAD_WITH_ALTERED_SEARCH_PATH : 0);
	return 0;
}
