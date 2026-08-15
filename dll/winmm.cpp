#include "winmm.h"

#include "common.h"
#include "context.h"
#include "kernel32/sysinfoapi.h"
#include "modules.h"

namespace winmm {

DWORD WINAPI timeGetTime() {
	HOST_CONTEXT_GUARD();
	const DWORD result = kernel32::GetTickCount();
	DEBUG_LOG("timeGetTime() -> %u\n", result);
	return result;
}

} // namespace winmm

#include "winmm_trampolines.h"

extern const wibo::ModuleStub lib_winmm = {
	(const char *[]){"winmm", nullptr},
	winmmThunkByName,
	nullptr,
};
