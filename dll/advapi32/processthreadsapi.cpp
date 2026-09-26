#include "processthreadsapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "handles.h"
#include "internal.h"
#include "kernel32/internal.h"
#include "processes.h"

namespace advapi32 {

BOOL WINAPI OpenProcessToken(HANDLE ProcessHandle, DWORD DesiredAccess, PHANDLE TokenHandle) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("OpenProcessToken(%p, %u, %p)\n", ProcessHandle, DesiredAccess, TokenHandle);
	if (!TokenHandle) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	Pin<ProcessObject> obj;
	if (kernel32::isPseudoCurrentProcessHandle(ProcessHandle)) {
		obj = make_pin<ProcessObject>(getpid(), -1, false);
	} else {
		obj = wibo::handles().getAs<ProcessObject>(ProcessHandle);
	}
	if (!obj) {
		kernel32::setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	auto token = make_pin<TokenObject>(std::move(obj), DesiredAccess);
	*TokenHandle = wibo::handles().alloc(std::move(token), 0, 0);
	return TRUE;
}

BOOL WINAPI OpenThreadToken(HANDLE thread, DWORD desiredAccess, BOOL openAsSelf, PHANDLE token) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("OpenThreadToken(%p, 0x%x, %d, %p)\n", thread, desiredAccess, openAsSelf, token);
	if (!token) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*token = NO_HANDLE;
	if (!kernel32::isPseudoCurrentThreadHandle(thread) && !wibo::handles().getAs<kernel32::ThreadObject>(thread)) {
		kernel32::setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	// Guest threads have no impersonation token; token assignment is not implemented.
	kernel32::setLastError(ERROR_NO_TOKEN);
	return FALSE;
}

} // namespace advapi32
