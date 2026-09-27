#define WIN32_LEAN_AND_MEAN
#include <windows.h>

BOOL WINAPI shadowEntry(HINSTANCE instance, DWORD reason, LPVOID reserved) {
	(void)instance;
	(void)reason;
	(void)reserved;
	return TRUE;
}

__declspec(dllexport) int core_shadow_marker(void) { return 73; }
