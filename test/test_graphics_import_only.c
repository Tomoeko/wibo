#include <d3d9.h>
#include <string.h>
#include <windows.h>

#include "test_assert.h"

int main(int argc, char **argv) {
	HMODULE module = GetModuleHandleA("d3d9.dll");
	TEST_CHECK(module != NULL);
	IDirect3D9 *(WINAPI *volatile create)(UINT) = Direct3DCreate9;
	TEST_CHECK(create != NULL);
	HDC(WINAPI *volatile createContext)(HDC) = CreateCompatibleDC;
	TEST_CHECK(createContext != NULL);
	HMODULE graphics = GetModuleHandleA("gdi32.dll");
	TEST_CHECK(graphics != NULL && graphics != module);
	HMODULE loaded = LoadLibraryA("d3d9.dll");
	TEST_CHECK(loaded == module);
	TEST_CHECK(FreeLibrary(loaded));
	if (argc == 2 && strcmp(argv[1], "--call") == 0) {
		create(D3D_SDK_VERSION);
		return 21;
	}
	TEST_CHECK_EQ(1, argc);
	return 0;
}
