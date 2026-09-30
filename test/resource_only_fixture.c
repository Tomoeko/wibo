#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
	(void)instance;
	(void)reason;
	(void)reserved;
	return FALSE;
}

__declspec(dllexport) int fixture_marker(void) { return 7; }
