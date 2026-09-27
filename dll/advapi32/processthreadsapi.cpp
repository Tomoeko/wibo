#include "processthreadsapi.h"

#include "common.h"
#include "context.h"
#include "errors.h"
#include "handles.h"
#include "internal.h"
#include "kernel32/internal.h"
#include "kernel32/processthreadsapi.h"
#include "processes.h"

namespace advapi32 {

BOOL WINAPI OpenProcessToken(HANDLE ProcessHandle, DWORD DesiredAccess, PHANDLE TokenHandle) {
	HOST_CONTEXT_GUARD();
	DEBUG_LOG("OpenProcessToken(%p, %u, %p)\n", ProcessHandle, DesiredAccess, TokenHandle);
	if (!TokenHandle) {
		kernel32::setLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*TokenHandle = NO_HANDLE;
	Pin<ProcessObject> obj;
	if (kernel32::isPseudoCurrentProcessHandle(ProcessHandle)) {
		obj = make_pin<ProcessObject>(getpid(), -1, false);
	} else {
		HandleMeta metadata{};
		obj = wibo::handles().getAs<ProcessObject>(ProcessHandle, &metadata);
		if (obj && !(metadata.grantedAccess & (PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION))) {
			kernel32::setLastError(ERROR_ACCESS_DENIED);
			return FALSE;
		}
	}
	if (!obj) {
		kernel32::setLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (obj->pid != getpid()) {
		kernel32::setLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	DWORD granted = 0;
	const DWORD error = tokenAccessError(DesiredAccess, granted);
	if (error != ERROR_SUCCESS || granted == 0) {
		kernel32::setLastError(error != ERROR_SUCCESS ? error : ERROR_ACCESS_DENIED);
		return FALSE;
	}
	auto token = make_pin<TokenObject>(std::move(obj), wibo::identity::currentTokenIdentityContext());
	*TokenHandle = wibo::handles().alloc(std::move(token), granted, 0);
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
