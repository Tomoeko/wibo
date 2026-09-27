#include <windows.h>

__declspec(dllimport) int cycle_value_b(void);

__declspec(dllexport) int cycle_value_a(void) { return cycle_value_b() + 1; }

typedef void (*record_fn)(int, int, int);

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
	(void)instance;
	(void)reserved;
	if (reason == DLL_PROCESS_ATTACH) {
		char reject[2];
		return !GetEnvironmentVariableA("WIBO_LIFETIME_REJECT_ATTACH", reject, sizeof(reject));
	}
	if (reason == DLL_PROCESS_DETACH) {
		record_fn record = (record_fn)(ULONG_PTR)GetProcAddress(GetModuleHandleA(NULL), "lifetime_detach_record");
		if (record)
			record(0, cycle_value_b(), GetModuleHandleA("module_lifetime_cycle_b.dll") != NULL);
	}
	return TRUE;
}
