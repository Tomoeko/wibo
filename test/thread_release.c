#include <windows.h>

static HMODULE module;
static LONG *detachCount;

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH) {
		module = instance;
		detachCount = NULL;
	} else if (reason == DLL_PROCESS_DETACH && detachCount) {
		InterlockedIncrement(detachCount);
	}
	return TRUE;
}

__declspec(dllexport) DWORD WINAPI release_worker(LPVOID counter) {
	detachCount = counter;
	FreeLibraryAndExitThread(module, 37);
}
