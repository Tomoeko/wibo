#include <windows.h>

__declspec(dllimport) int cycle_value_a(void);

__declspec(dllexport) int cycle_value_b(void) { return 37; }

__declspec(dllexport) int cycle_back_reference(void) { return cycle_value_a(); }

typedef void (*record_fn)(int, int, int);

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
	(void)instance;
	(void)reserved;
	if (reason == DLL_PROCESS_DETACH) {
		record_fn record = (record_fn)(ULONG_PTR)GetProcAddress(GetModuleHandleA(NULL), "lifetime_detach_record");
		if (record)
			record(1, cycle_value_a(), GetModuleHandleA("module_lifetime_cycle_a.dll") != NULL);
	}
	return TRUE;
}
