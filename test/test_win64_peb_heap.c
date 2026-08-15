#include <stdint.h>
#include <windows.h>

static uintptr_t read_teb_self(void) {
	uintptr_t self;
	__asm__("movq %%gs:0x30, %0" : "=r"(self));
	return self;
}

// Match the access pattern used by Wine's msvcrt!__wgetmainargs: preserve the
// TEB self pointer across a host API call, then derive PEB->ProcessHeap from it.
void mainCRTStartup(void) {
	uintptr_t teb = read_teb_self();
	LPWCH environment = GetEnvironmentStringsW();
	if (!environment) {
		ExitProcess(1);
	}

	uintptr_t peb = *(uintptr_t *)(teb + 0x60);
	HANDLE processHeap = *(HANDLE *)(peb + 0x30);
	if (!processHeap) {
		ExitProcess(2);
	}

	void *allocation = HeapAlloc(processHeap, 0, 8402);
	if (!allocation) {
		ExitProcess(3);
	}

	if (!HeapFree(processHeap, 0, allocation)) {
		ExitProcess(4);
	}
	if (!FreeEnvironmentStringsW(environment)) {
		ExitProcess(5);
	}
	ExitProcess(0);
}
