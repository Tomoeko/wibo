#include "debugapi.h"

#include "common.h"
#include "context.h"

namespace kernel32 {

BOOL WINAPI IsDebuggerPresent() {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("STUB: IsDebuggerPresent()\n");
	return FALSE;
}

VOID WINAPI OutputDebugStringA(LPCSTR lpOutputString) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("OutputDebugStringA(%s)\n", lpOutputString ? lpOutputString : "<null>");
}

VOID WINAPI OutputDebugStringW(LPCWSTR lpOutputString) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("OutputDebugStringW(%p)\n", lpOutputString);
}

WORD WINAPI RtlCaptureStackBackTrace(DWORD FramesToSkip, DWORD FramesToCapture, PVOID *BackTrace,
									 PDWORD BackTraceHash) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("RtlCaptureStackBackTrace(%u, %u, %p, %p)\n", FramesToSkip, FramesToCapture, BackTrace,
			  BackTraceHash);
	if (BackTraceHash) {
		*BackTraceHash = 0;
	}
	// Host return addresses are not valid PE addresses and feeding them to the
	// guest's DbgHelp path is actively misleading. Report an empty trace until
	// Wibo has a Windows unwind-table walker.
	return 0;
}

} // namespace kernel32
