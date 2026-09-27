#include <windows.h>

static int *observations;
static HMODULE *retained;
static BOOL from_address;

__declspec(dllexport) void detach_configure(int *state, HMODULE *reference, BOOL use_address) {
	observations = state;
	retained = reference;
	from_address = use_address;
	state[0] = 1;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
	(void)instance;
	(void)reserved;
	if (!observations)
		return TRUE;
	if (reason == DLL_PROCESS_ATTACH)
		++observations[0];
	if (reason == DLL_PROCESS_DETACH) {
		++observations[1];
		observations[2] = GetModuleHandleA("module_detach_reference.dll") != NULL;
		if (retained && !*retained) {
			DWORD flags = from_address ? GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS : 0;
			LPCSTR name = from_address ? (LPCSTR)(ULONG_PTR)&detach_configure : "module_detach_reference.dll";
			if (!GetModuleHandleExA(flags, name, retained))
				observations[2] = -1;
		}
	}
	return TRUE;
}
