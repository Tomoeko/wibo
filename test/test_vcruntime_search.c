#include <string.h>
#include <windows.h>

#include "test_assert.h"

typedef void *(__cdecl *MemchrFn)(const void *buffer, int value, size_t count);
typedef char *(__cdecl *StrstrFn)(const char *text, const char *needle);
typedef char *(__cdecl *StrrchrFn)(const char *text, int value);
typedef char *(__cdecl *StrchrFn)(const char *text, int value);

int main(void) {
	HMODULE runtime = LoadLibraryA("vcruntime140.dll");
	TEST_CHECK(runtime != NULL);
	FARPROC procedure = GetProcAddress(runtime, "memchr");
	TEST_CHECK(procedure != NULL);
	MemchrFn search;
	TEST_CHECK(sizeof(search) == sizeof(procedure));
	memcpy(&search, &procedure, sizeof(search));

	unsigned char bytes[] = {0x80, 0x00, 0xff, 0x53, 0x80, 0x7f};
	TEST_CHECK(search(bytes, 0x80, sizeof(bytes)) == bytes);
	TEST_CHECK(search(bytes, 0, sizeof(bytes)) == bytes + 1);
	TEST_CHECK(search(bytes, -1, sizeof(bytes)) == bytes + 2);
	TEST_CHECK(search(bytes, 0x180, sizeof(bytes)) == bytes);
	TEST_CHECK(search(bytes, 0x7f, sizeof(bytes)) == bytes + 5);
	TEST_CHECK(search(bytes, 0x7f, sizeof(bytes) - 1) == NULL);
	TEST_CHECK(search(bytes, 0x80, 0) == NULL);
	TEST_CHECK(search(bytes + 3, 0x80, 2) == bytes + 4);
	TEST_CHECK(search(bytes, 0x54, sizeof(bytes)) == NULL);
	const unsigned char expected[] = {0x80, 0x00, 0xff, 0x53, 0x80, 0x7f};
	TEST_CHECK(memcmp(bytes, expected, sizeof(bytes)) == 0);
	procedure = GetProcAddress(runtime, "strstr");
	TEST_CHECK(procedure != NULL);
	StrstrFn find;
	TEST_CHECK(sizeof(find) == sizeof(procedure));
	memcpy(&find, &procedure, sizeof(find));
	const char text[] = "ababa\0hidden";
	TEST_CHECK(find(text, "aba") == text);
	TEST_CHECK(find(text, "ba") == text + 1);
	TEST_CHECK(find(text, "") == text);
	TEST_CHECK(find(text, "ababa") == text);
	TEST_CHECK(find(text, "ababaa") == NULL);
	TEST_CHECK(find(text, "hidden") == NULL);
	TEST_CHECK(find(text, "z") == NULL);
	const char empty[] = "";
	TEST_CHECK(find(empty, "") == empty);
	TEST_CHECK(find(empty, "a") == NULL);
	procedure = GetProcAddress(runtime, "strrchr");
	TEST_CHECK(procedure != NULL);
	StrrchrFn findLast;
	TEST_CHECK(sizeof(findLast) == sizeof(procedure));
	memcpy(&findLast, &procedure, sizeof(findLast));
	TEST_CHECK(findLast(text, 'a') == text + 4);
	TEST_CHECK(findLast(text, 'b') == text + 3);
	TEST_CHECK(findLast(text, 0) == text + 5);
	TEST_CHECK(findLast(text, 0x161) == text + 4);
	TEST_CHECK(findLast(text, 'z') == NULL);
	TEST_CHECK(findLast(empty, 0) == empty);
	TEST_CHECK(findLast(empty, 'a') == NULL);
	procedure = GetProcAddress(runtime, "strchr");
	TEST_CHECK(procedure != NULL);
	StrchrFn findFirst;
	TEST_CHECK(sizeof(findFirst) == sizeof(procedure));
	memcpy(&findFirst, &procedure, sizeof(findFirst));
	TEST_CHECK(findFirst(text, 'a') == text);
	TEST_CHECK(findFirst(text, 'b') == text + 1);
	TEST_CHECK(findFirst(text, 0) == text + 5);
	TEST_CHECK(findFirst(text, 0x161) == text);
	TEST_CHECK(findFirst(text, 0x100) == text + 5);
	TEST_CHECK(findFirst(text, 'h') == NULL);
	TEST_CHECK(findFirst(empty, 0) == empty);
	TEST_CHECK(findFirst(empty, 'a') == NULL);
	const char highBytes[] = {(char)0x80, (char)0xff, (char)0x80, 0};
	TEST_CHECK(findFirst(highBytes, -128) == highBytes);
	TEST_CHECK(findFirst(highBytes, 0x180) == highBytes);
	TEST_CHECK(findFirst(highBytes, 0x1ff) == highBytes + 1);
	TEST_CHECK(findFirst(highBytes, 0) == highBytes + 3);
	SYSTEM_INFO systemInfo;
	GetSystemInfo(&systemInfo);
	SIZE_T pageSize = systemInfo.dwPageSize;
	char *guarded = VirtualAlloc(NULL, pageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	TEST_CHECK(guarded != NULL);
	DWORD previousProtection;
	TEST_CHECK(VirtualProtect(guarded + pageSize, pageSize, PAGE_NOACCESS, &previousProtection));
	guarded[pageSize - 2] = 'x';
	guarded[pageSize - 1] = 0;
	TEST_CHECK(findFirst(guarded + pageSize - 2, 'x') == guarded + pageSize - 2);
	TEST_CHECK(findFirst(guarded + pageSize - 2, 'y') == NULL);
	TEST_CHECK(findFirst(guarded + pageSize - 2, 0) == guarded + pageSize - 1);
	TEST_CHECK(findFirst(guarded + pageSize - 1, 'x') == NULL);
	TEST_CHECK(findFirst(guarded + pageSize - 1, 0) == guarded + pageSize - 1);
	TEST_CHECK(VirtualFree(guarded, 0, MEM_RELEASE));
	TEST_CHECK(FreeLibrary(runtime));
	return 0;
}
