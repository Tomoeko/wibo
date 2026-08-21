#include "winmm.h"

#include "common.h"
#include "context.h"
#include "kernel32/sysinfoapi.h"
#include "modules.h"

namespace winmm {

UINT WINAPI timeBeginPeriod(UINT uPeriod) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("timeBeginPeriod(%u)\n", uPeriod);
	// Host wait primitives already provide sub-millisecond resolution. Keep the
	// WinMM request process-local and side-effect free instead of changing a
	// system-wide host timer policy.
	return uPeriod == 0 ? 97 /* TIMERR_NOCANDO */ : 0 /* TIMERR_NOERROR */;
}

UINT WINAPI timeEndPeriod(UINT uPeriod) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("timeEndPeriod(%u)\n", uPeriod);
	return uPeriod == 0 ? 97 /* TIMERR_NOCANDO */ : 0 /* TIMERR_NOERROR */;
}

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
