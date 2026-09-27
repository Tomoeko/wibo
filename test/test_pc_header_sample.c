#include <windows.h>

__declspec(dllexport) DWORD WINAPI SampleValue(void) { return 42; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
	(void)instance;
	(void)reason;
	(void)reserved;
	return TRUE;
}
