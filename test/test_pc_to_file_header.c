#include <windows.h>

#include "test_assert.h"

typedef PVOID(WINAPI *pc_header_fn)(PVOID, PVOID *);

typedef struct {
	BYTE before[8];
	PVOID value;
	BYTE after[8];
} GuardedBase;

static pc_header_fn functions[2];

static void checkAddress(const char *label, PVOID address, PVOID expected) {
	for (unsigned api = 0; api < 2; ++api) {
		GuardedBase guarded;
		memset(&guarded, 0xa5, sizeof(guarded));
		guarded.value = (PVOID)(ULONG_PTR)0x12345678;
		SetLastError(0x4321);
		PVOID result = functions[api](address, &guarded.value);
		DWORD error = GetLastError();
		printf("api=%u case=%s result=%p output=%p expected=%p error=%lu\n", api, label, result, guarded.value,
			   expected, (unsigned long)error);
		TEST_CHECK(result == expected);
		TEST_CHECK(guarded.value == expected);
		TEST_CHECK_EQ(0x4321, error);
		for (unsigned index = 0; index < sizeof(guarded.before); ++index) {
			TEST_CHECK_EQ(0xa5, guarded.before[index]);
			TEST_CHECK_EQ(0xa5, guarded.after[index]);
		}
	}
}

int main(void) {
	const char *names[] = {"kernel32.dll", "ntdll.dll"};
	for (unsigned api = 0; api < 2; ++api) {
		HMODULE module = GetModuleHandleA(names[api]);
		TEST_CHECK(module != NULL);
		FARPROC symbol = GetProcAddress(module, "RtlPcToFileHeader");
		TEST_CHECK(sizeof(functions[api]) == sizeof(symbol));
		memcpy(&functions[api], &symbol, sizeof(symbol));
		TEST_CHECK(functions[api] != NULL);
	}
	HMODULE mainModule = GetModuleHandleA(NULL);
	TEST_CHECK(mainModule != NULL);
	checkAddress("main-image", (PVOID)(ULONG_PTR)&main, mainModule);
	checkAddress("image-base", mainModule, mainModule);
	checkAddress("null", NULL, NULL);
	checkAddress("unmapped", (PVOID)(ULONG_PTR)1, NULL);
	PVOID allocation = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	TEST_CHECK(allocation != NULL);
	checkAddress("nonimage-allocation", allocation, NULL);
	TEST_CHECK(VirtualFree(allocation, 0, MEM_RELEASE));
	char path[MAX_PATH];
	DWORD length = GetModuleFileNameA(NULL, path, sizeof(path));
	TEST_CHECK(length > 0 && length < sizeof(path));
	char *part = path;
	for (char *cursor = path; *cursor; ++cursor)
		if (*cursor == '\\' || *cursor == '/')
			part = cursor + 1;
	const char name[] = "pc-header-sample.dll";
	TEST_CHECK((size_t)(part - path) + sizeof(name) <= sizeof(path));
	memcpy(part, name, sizeof(name));
	HMODULE dynamicModule = LoadLibraryA(path);
	TEST_CHECK(dynamicModule != NULL);
	FARPROC exported = GetProcAddress(dynamicModule, "SampleValue");
	TEST_CHECK(exported != NULL);
	PVOID address = NULL;
	TEST_CHECK(sizeof(address) == sizeof(exported));
	memcpy(&address, &exported, sizeof(address));
	checkAddress("dynamic-function", address, dynamicModule);
	checkAddress("dynamic-base", dynamicModule, dynamicModule);
	TEST_CHECK(FreeLibrary(dynamicModule));
	TEST_CHECK(GetModuleHandleA("pc-header-sample.dll") == NULL);
	checkAddress("unloaded-function", address, NULL);
	return 0;
}
